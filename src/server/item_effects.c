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

/*item_effects.c - The worn effects of magic items and the use effects of
 * consumables (LORE.md, "Magic items"; item_effects.h).*/

#include "item_effects.h"

#include "inventory.h"
#include "player.h"
#include "server_internal.h"
#include "spells.h"
#include "classes.h"
#include "spell_rules.h"
#include "server_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*The damage types of the d10 table of the potion and ring of resistance.*/
static const DamageType k_resist_table[10] = {DMG_ACID,      DMG_COLD,     DMG_FIRE,   DMG_FORCE,
                                              DMG_LIGHTNING, DMG_NECROTIC, DMG_POISON, DMG_PSYCHIC,
                                              DMG_RADIANT,   DMG_THUNDER};

/*Names of the timed effects of the consumables: rules_add_effect merges
 * effects of the same name (the best value and the longest duration),
 * so each kind has its own.*/
static const char *const k_name_attack = "Magic: attack";
static const char *const k_name_damage = "Magic: damage";
static const char *const k_name_shrunk = "Magic: shrunk";
static const char *const k_name_armour = "Magic: armour";
static const char *const k_name_saves = "Magic: saves";
static const char *const k_name_sight = "Magic: sight";
static const char *const k_name_haste = "Magic: haste";
static const char *const k_name_resist = "Magic: resistance";
static const char *const k_name_warding = "Magic: warding";
static const char *const k_name_temp_hp = "Temporary HP";

static bool lit_or_not_light(const ItemInstance *it, const ItemTemplate *t) {
    return t->category != ITEM_LIGHT_SOURCE || it->durability > 0;
}

int player_active_items(Client *c, ItemInstance **out) {
    int n = 0;
    for (int i = 0; i < EQUIP_SLOTS_ALL; i++) {
        ItemInstance *s =
            i < EQUIP_SLOT_COUNT ? equip_slot(c, i) : &c->slot_rings[i - EQUIP_SLOT_COUNT];
        if (s->template_idx >= 0 && lit_or_not_light(s, &item_database[s->template_idx])) {
            out[n++] = s;
        }
    }
    for (int i = 0; i < MAX_BACKPACK + MAX_BELT; i++) {
        ItemInstance *s = i < MAX_BACKPACK ? &c->backpack[i] : &c->belt[i - MAX_BACKPACK];
        if ((i < MAX_BACKPACK && i >= c->backpack_count) || s->template_idx < 0 ||
            !s->is_identified || !item_database[s->template_idx].worn.carried) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) {
            dup = out[k]->template_idx == s->template_idx;
        }
        if (!dup) {
            out[n++] = s;
        }
    }
    return n;
}

uint32_t item_resist_mask(ItemInstance *it) {
    const ItemWorn *w = &item_database[it->template_idx].worn;
    uint32_t mask = w->resist;
    if (w->resist_random) {
        item_roll_variant(it);
        mask |= 1u << (it->current_charges - 1);
    }
    return mask;
}

void item_roll_variant(ItemInstance *it) {
    if (it->template_idx < 0 || !item_database[it->template_idx].worn.resist_random) {
        return;
    }
    if (it->current_charges >= 1 && it->current_charges <= MAX_DAMAGE_TYPES) {
        return;
    }
    uint32_t choices = item_database[it->template_idx].worn.resist_choices;
    DamageType pick[MAX_DAMAGE_TYPES];
    int n = 0;
    for (int t = 0; t < MAX_DAMAGE_TYPES; t++) {
        if (choices & (1u << t)) {
            pick[n++] = (DamageType)t;
        }
    }
    it->current_charges = (int)(n > 0 ? pick[rand() % n] : k_resist_table[rand() % 10]) + 1;
}

static void worn_flags(WornTotals *w, const ItemWorn *t) {
    w->haste |= t->haste;
    w->displacement |= t->displacement;
    w->see_invisible |= t->see_invisible;
    w->spell_save_advantage |= t->spell_save_advantage;
    w->gas_save_advantage |= t->gas_save_advantage;
    w->feather_fall |= t->feather_fall;
    w->sustenance |= t->sustenance;
    w->crit_immune |= t->crit_immune;
}

void player_worn(Client *c, WornTotals *w) {
    memset(w, 0, sizeof(*w));
    ItemInstance *items[MAX_ACTIVE_ITEMS];
    int n = player_active_items(c, items);
    for (int i = 0; i < n; i++) {
        const ItemWorn *t = &item_database[items[i]->template_idx].worn;
        if (!t->any) {
            continue;
        }
        w->ac += t->ac;
        w->ac_unarmored += t->ac_unarmored;
        if (t->unarmored_base > w->unarmored_base) {
            w->unarmored_base = t->unarmored_base;
        }
        w->saves += t->saves;
        w->ranged_damage += t->ranged_damage;
        w->spell_dc += t->spell_dc;
        w->spell_attack += t->spell_attack;
        if (t->darkvision > w->darkvision) {
            w->darkvision = t->darkvision;
        }
        w->resist |= item_resist_mask(items[i]);
        w->immune |= t->immune;
        w->immune_conditions |= t->immune_conditions;
        worn_flags(w, t);
    }
}

/*The value of the timed effects with this trigger, ORed (the masks).*/
static uint32_t effect_mask(const Client *c, RuleEventType trigger) {
    uint32_t mask = 0;
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].trigger == trigger && c->effects[i].duration_rounds > 0) {
            mask |= (uint32_t)c->effects[i].value;
        }
    }
    return mask;
}

static bool has_trigger(const Client *c, RuleEventType trigger) {
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].trigger == trigger && c->effects[i].duration_rounds > 0) {
            return true;
        }
    }
    return false;
}

/*True if the character's class is proficient in the saves of a (LORE.md,
 * the classes: "Saves").*/
static bool save_proficient(const Client *c, Ability a) {
    if (c->class_id < 0 || c->class_id >= CLASS_COUNT) {
        return false;
    }
    const ClassTemplate *k = &CLASSES[c->class_id];
    const int prof[ABILITY_COUNT] = {k->str_save, k->dex_save, k->con_save,
                                     k->int_save, k->wis_save, k->cha_save};
    return a >= ABILITY_STR && a < ABILITY_COUNT && prof[a];
}

int player_save_mod(Client *c, Ability a) {
    int s[ABILITY_COUNT];
    get_total_stats(c, &s[ABILITY_STR], &s[ABILITY_DEX], &s[ABILITY_CON], &s[ABILITY_INT],
                    &s[ABILITY_WIS], &s[ABILITY_CHA]);
    WornTotals w;
    player_worn(c, &w);
    int mod = (a >= ABILITY_STR && a < ABILITY_COUNT) ? rules_get_modifier(s[a]) : 0;
    if (save_proficient(c, a)) {
        mod += spell_proficiency(c->level);
    }
    return mod + w.saves + rules_event_bonus(c->effects, c->effect_count, EVENT_ON_SAVE);
}

/*A worn ring of evasion with a charge left spends it; true if it did.*/
static bool spend_evasion(Client *c) {
    ItemInstance *items[MAX_ACTIVE_ITEMS];
    int n = player_active_items(c, items);
    for (int i = 0; i < n; i++) {
        const ItemTemplate *t = &item_database[items[i]->template_idx];
        if (t->worn.evasion && items[i]->current_charges > 0) {
            items[i]->current_charges--;
            send_text_to_client(c->sock, "[MAGIC] Your %s flares: you evade! (%d charges left)",
                                t->name, items[i]->current_charges);
            return true;
        }
    }
    return false;
}

bool player_save(Client *c, Ability a, int dc, SaveSource src, int *roll) {
    WornTotals w;
    player_worn(c, &w);
    bool adv =
        (src == SAVE_MAGIC && w.spell_save_advantage) || (src == SAVE_GAS && w.gas_save_advantage);
    bool dis = rules_has_condition_t(c->effects, c->effect_count, COND_CURSED);
    bool ok = rules_roll_save(player_save_mod(c, a), dc, adv, dis, roll);
    if (!ok && a == ABILITY_DEX) {
        ok = spend_evasion(c);
    }
    return ok;
}

/*Immune (2), resistant (1) or neither (0) to damage of this type.*/
static int damage_protection(Client *c, int type) {
    WornTotals w;
    player_worn(c, &w);
    uint32_t resist = w.resist | effect_mask(c, EVENT_ON_RESIST);
    if (type == DAMAGE_PHYSICAL) {
        uint32_t phys = (1u << DMG_BLUDGEONING) | (1u << DMG_PIERCING) | (1u << DMG_SLASHING);
        if ((w.immune & phys) == phys) {
            return 2;
        }
        return (resist & phys) == phys ? 1 : 0;
    }
    if (type < 0 || type >= MAX_DAMAGE_TYPES) {
        return 0;
    }
    if (w.immune & (1u << type)) {
        return 2;
    }
    return (resist & (1u << type)) ? 1 : 0;
}

/*Temporary hit points take the damage first; returns what is left.*/
static int absorb_temp_hp(Client *c, int dmg) {
    for (int i = 0; i < c->effect_count && dmg > 0; i++) {
        ActiveEffect *e = &c->effects[i];
        if (e->trigger != EVENT_ON_TEMP_HP || e->value <= 0) {
            continue;
        }
        int took = dmg < e->value ? dmg : e->value;
        e->value -= took;
        dmg -= took;
        send_text_to_client(c->sock,
                            "[MAGIC] Your temporary hit points absorb %d damage (%d left).", took,
                            e->value);
        if (e->value <= 0) {
            e->duration_rounds = 0; /*gone at the next round*/
        }
    }
    return dmg;
}

int player_damage(Client *c, int dmg, int type) {
    if (dmg <= 0) {
        return 0;
    }
    c->last_combat_ms = get_time_ms(); /*hurt: in a fight (client_in_combat)*/
    int prot = damage_protection(c, type);
    if (prot == 2) {
        send_text_to_client(c->sock, "[MAGIC] You are immune: the damage has no effect on you.");
        return 0;
    }
    if (prot == 1) {
        dmg /= 2;
        send_text_to_client(c->sock, "[MAGIC] You resist: the damage is halved.");
    }
    c->hp -= absorb_temp_hp(c, dmg);
    return dmg;
}

bool player_condition_immune(Client *c, ConditionType cond) {
    if ((int)cond < 0 || cond >= MAX_CONDITIONS) {
        return false;
    }
    WornTotals w;
    player_worn(c, &w);
    uint32_t mask = w.immune_conditions | effect_mask(c, EVENT_ON_IMMUNE);
    if (cond == COND_POISONED && (w.immune & (1u << DMG_POISON))) {
        mask |= 1u << COND_POISONED; /*poisons have no effect on who is immune to poison*/
    }
    return (mask & (1u << cond)) != 0;
}

bool player_add_condition(Client *c, ConditionType cond, int rounds, int value) {
    if (player_condition_immune(c, cond)) {
        send_text_to_client(c->sock, "[MAGIC] Your magic protects you: you are not %s.",
                            condition_to_name(cond));
        return false;
    }
    ActiveEffect e = {
        condition_to_name(cond), EVENT_ON_TURN_START, MOD_ADDITIVE, value, rounds, false};
    return rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &e);
}

bool player_spell_immune(Client *c, const char *spell) {
    ItemInstance *items[MAX_ACTIVE_ITEMS];
    int n = player_active_items(c, items);
    for (int i = 0; i < n; i++) {
        int idx = item_database[items[i]->template_idx].worn.immune_spell;
        if (idx >= 0 && idx < spell_database_size &&
            strcasecmp(spell_database[idx].name, spell) == 0) {
            send_text_to_client(c->sock, "[MAGIC] Your %s stops the %s.",
                                item_database[items[i]->template_idx].name, spell);
            return true;
        }
    }
    return false;
}

bool player_hasted(Client *c) {
    WornTotals w;
    player_worn(c, &w);
    return w.haste || has_trigger(c, EVENT_ON_HASTE);
}

bool player_sees_invisible(Client *c) {
    WornTotals w;
    player_worn(c, &w);
    if (w.see_invisible) {
        return true;
    }
    for (int i = 0; i < MAX_BELT; i++) { /*a lit lantern of revealing on the belt*/
        const ItemInstance *b = &c->belt[i];
        if (b->template_idx >= 0 && b->durability > 0 &&
            item_database[b->template_idx].category == ITEM_LIGHT_SOURCE &&
            item_database[b->template_idx].worn.see_invisible) {
            return true;
        }
    }
    return false;
}

void player_spell_item_bonus(Client *c, int *dc, int *attack) {
    WornTotals w;
    player_worn(c, &w);
    *dc = w.spell_dc;
    *attack = w.spell_attack;
}

static int roll_dice(const ItemDice *d) {
    return (d->count > 0 && d->sides > 0 ? rules_roll_dice(d->count, d->sides) : 0) + d->bonus;
}

void player_items_round(Game *game, Client *c) {
    if (c->hp <= 0 || c->hp >= c->max_hp) {
        return;
    }
    ItemInstance *items[MAX_ACTIVE_ITEMS];
    int n = player_active_items(c, items);
    for (int i = 0; i < n; i++) {
        const ItemTemplate *t = &item_database[items[i]->template_idx];
        if (t->worn.regen_rounds <= 0 || game->total_turns % t->worn.regen_rounds != 0) {
            continue;
        }
        int heal = roll_dice(&t->worn.regen);
        c->hp = c->hp + heal > c->max_hp ? c->max_hp : c->hp + heal;
        send_text_to_client(c->sock, "[MAGIC] Your %s mends your wounds: +%d HP (HP: %d/%d).",
                            t->name, heal, c->hp, c->max_hp);
        send_detailed_state(game, c);
    }
}

/*A timed effect of a consumable; masks (resistance, warding) are ORed
 * into an effect of the same name instead of replacing it.*/
static void add_timed(Client *c, const char *name, RuleEventType trigger, int value, int rounds,
                      bool mask) {
    for (int i = 0; mask && i < c->effect_count; i++) {
        ActiveEffect *e = &c->effects[i];
        if (e->name && strcmp(e->name, name) == 0) {
            e->value |= value;
            if (rounds > e->duration_rounds) {
                e->duration_rounds = rounds;
            }
            return;
        }
    }
    ActiveEffect e = {name, trigger, MOD_ADDITIVE, value, rounds, false};
    if (!rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &e)) {
        send_text_to_client(c->sock, "[ITEM] Too much magic on you already: part of it fades.");
    }
}

/*Drops every effect of the conditions in mask; true if any went.*/
static bool cure_conditions(Client *c, uint32_t mask) {
    bool any = false;
    int i = 0;
    while (i < c->effect_count) {
        ConditionType cond = condition_from_name(c->effects[i].name);
        if (cond < MAX_CONDITIONS && (mask & (1u << cond))) {
            c->effects[i] = c->effects[--c->effect_count];
            any = true;
        } else {
            i++;
        }
    }
    return any;
}

static ItemInstance *weapon_in_hand(Client *c) {
    ItemInstance *h[] = {&c->slot_hand_r, &c->slot_hand_l};
    for (int i = 0; i < 2; i++) {
        if (h[i]->template_idx >= 0 && item_database[h[i]->template_idx].category == ITEM_WEAPON) {
            return h[i];
        }
    }
    return NULL;
}

/*The harm of a poison and its condition (with its saving throw).*/
static void use_harm(Client *c, const ItemTemplate *t, int rounds) {
    const ItemUse *u = &t->use;
    if (u->harm.count > 0 || u->harm.bonus > 0) {
        int dealt = player_damage(c, roll_dice(&u->harm), (int)u->harm_type);
        send_text_to_client(c->sock, "[ITEM] %s burns inside you: %d %s damage! (HP: %d/%d)",
                            t->name, dealt, damage_type_to_name(u->harm_type),
                            c->hp > 0 ? c->hp : 0, c->max_hp);
    }
    if (u->condition >= MAX_CONDITIONS) {
        return;
    }
    if (u->save_ability != ABILITY_NONE &&
        player_save(c, u->save_ability, u->save_dc, SAVE_PLAIN, NULL)) {
        send_text_to_client(c->sock, "[ITEM] You resist: you are not %s.",
                            condition_to_name(u->condition));
        return;
    }
    if (player_add_condition(c, u->condition, rounds, 0)) {
        send_text_to_client(c->sock, "[ITEM] You are %s for %d rounds.",
                            condition_to_name(u->condition), rounds);
    }
}

/*The bonuses that last: attack, damage, armour, saves, sight, haste.*/
static void use_bonuses(Client *c, const ItemUse *u, int rounds) {
    if (u->attack) {
        add_timed(c, k_name_attack, EVENT_ON_ATTACK, u->attack, rounds, false);
    }
    if (u->damage) {
        add_timed(c, u->damage > 0 ? k_name_damage : k_name_shrunk, EVENT_ON_DAMAGE, u->damage,
                  rounds, false);
    }
    if (u->ac) {
        add_timed(c, k_name_armour, EVENT_ON_DEFEND, u->ac, rounds, false);
    }
    if (u->saves) {
        add_timed(c, k_name_saves, EVENT_ON_SAVE, u->saves, rounds, false);
    }
    if (u->darkvision) {
        add_timed(c, k_name_sight, EVENT_ON_LIGHT, u->darkvision, rounds, false);
    }
    if (u->haste) {
        add_timed(c, k_name_haste, EVENT_ON_HASTE, 1, rounds, false);
    }
    uint32_t resist = u->resist;
    if (u->resist_random) {
        DamageType type = k_resist_table[rand() % 10];
        resist |= 1u << type;
        send_text_to_client(c->sock, "[ITEM] You resist %s damage.", damage_type_to_name(type));
    }
    if (resist) {
        add_timed(c, k_name_resist, EVENT_ON_RESIST, (int)resist, rounds, true);
    }
    if (u->immune_conditions) {
        add_timed(c, k_name_warding, EVENT_ON_IMMUNE, (int)u->immune_conditions, rounds, true);
    }
    if (u->temp_hp > 0) {
        add_timed(c, k_name_temp_hp, EVENT_ON_TEMP_HP, u->temp_hp, rounds, false);
        send_text_to_client(c->sock, "[ITEM] You gain %d temporary hit points.", u->temp_hp);
    }
}

bool item_use_apply(Game *game, Client *c, const ItemTemplate *t) {
    const ItemUse *u = &t->use;
    if (u->weapon && !weapon_in_hand(c)) {
        send_text_to_client(c->sock, "[ERROR] %s must coat a weapon: hold one first.", t->name);
        return false;
    }
    int rounds = u->rounds;
    if (u->rounds_dice.count > 0) {
        rounds *= roll_dice(&u->rounds_dice);
    }
    if (u->cure && cure_conditions(c, u->cure)) {
        send_text_to_client(c->sock, "[ITEM] %s cleanses you.", t->name);
    }
    if (u->exhaustion && c->exhaustion_level > 0) {
        c->exhaustion_level = 0;
        send_text_to_client(c->sock, "[ITEM] Your exhaustion is gone.");
    }
    if (u->heal.count > 0 || u->heal.bonus > 0) {
        int healed = roll_dice(&u->heal);
        c->hp = c->hp + healed > c->max_hp ? c->max_hp : c->hp + healed;
        send_text_to_client(c->sock, "[ITEM] You drink %s and recover %d HP (HP: %d/%d).", t->name,
                            healed, c->hp, c->max_hp);
    }
    use_harm(c, t, rounds);
    use_bonuses(c, u, rounds);
    if (rounds > 0 && !u->heal.count) {
        send_text_to_client(c->sock, "[ITEM] You use %s: its magic lasts %d rounds.", t->name,
                            rounds);
    }
    send_detailed_state(game, c);
    return true;
}
