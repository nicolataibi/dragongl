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
 * player_stats.c - What a character is worth: total abilities, AC, weight, speed, vision,
 * item names and wear, experience and levels, starting gear.
 */

#include "server_internal.h"

#include "player.h"
#include "server_log.h"

/*Clamp a client-provided stat to the playable range 3..18.*/
int clamp_stat(int v) {
    if (v < 3) {
        return 3;
    }
    if (v > 18) {
        return 18;
    }
    return v;
}

void get_total_stats(Client *c, int *ts, int *td, int *tc, int *ti, int *tw, int *th) {
    *ts = c->str;
    *td = c->dex;
    *tc = c->con;
    *ti = c->intel;
    *tw = c->wis;
    *th = c->cha;
    ItemInstance *s[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                         &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_feet};
    for (int i = 0; i < 8; i++) {
        if (s[i]->template_idx != -1) {
            const ItemTemplate *it = &item_database[s[i]->template_idx];
            *ts += it->str_bonus;
            *td += it->dex_bonus;
            *tc += it->con_bonus;
            *ti += it->int_bonus;
            *tw += it->wis_bonus;
            *th += it->cha_bonus;
        }
    }
    for (int i = 0; i < 10; i++) {
        if (c->slot_rings[i].template_idx != -1) {
            const ItemTemplate *it = &item_database[c->slot_rings[i].template_idx];
            *ts += it->str_bonus;
            *td += it->dex_bonus;
            *tc += it->con_bonus;
            *ti += it->int_bonus;
            *tw += it->wis_bonus;
            *th += it->cha_bonus;
        }
    }
}

// -----------------------------------------------------------------------
static const char *blessing_word(ItemBlessing b) {
    switch (b) {
    case BLESS_BLESSED:
        return "Blessed ";
    case BLESS_CURSED:
        return "Cursed ";
    default:
        return "";
    }
}

static const char *quality_word(ItemQuality q) {
    switch (q) {
    case QUALITY_RUSTY:
        return "Rusty ";
    case QUALITY_FINE:
        return "Fine ";
    case QUALITY_MASTERWORK:
        return "Masterwork ";
    default:
        return "";
    }
}

static const char *element_word(ItemElement e) {
    switch (e) {
    case ELEM_FLAMING:
        return "Flaming ";
    case ELEM_FROST:
        return "Frost ";
    case ELEM_SHOCKING:
        return "Shocking ";
    case ELEM_POISONOUS:
        return "Poisonous ";
    default:
        return "";
    }
}

void get_full_item_name(const ItemInstance *inst, char *buf, size_t max_len) {
    if (!inst->is_identified) {
        snprintf(buf, max_len, "??? (unknown)");
        return;
    }
    //Unique Artifact: Uses its proper name directly
    if (inst->is_artifact && inst->artifact_name[0] != '\0') {
        const ItemTemplate *tmpl = &item_database[inst->template_idx];
        char suffix[64] = "";
        if (inst->to_hit_bonus != 0 || inst->to_dam_bonus != 0) {
            snprintf(suffix, sizeof(suffix), " (+%d, +%d)", inst->to_hit_bonus, inst->to_dam_bonus);
        } else if (inst->ac_bonus != 0) {
            snprintf(suffix, sizeof(suffix), " [+%d]", inst->ac_bonus);
        }
        snprintf(buf, max_len, "** %s [%s]%s **", inst->artifact_name, tmpl->name, suffix);
        return;
    }
    const ItemTemplate *tmpl = &item_database[inst->template_idx];
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s%s%s", blessing_word(inst->blessing),
             quality_word(inst->quality), element_word(inst->element));

    char suffix[64] = "";
    if (tmpl->category == ITEM_WEAPON && (inst->to_hit_bonus != 0 || inst->to_dam_bonus != 0)) {
        snprintf(suffix, sizeof(suffix), " (+%d, +%d)", inst->to_hit_bonus, inst->to_dam_bonus);
    } else if ((tmpl->category == ITEM_ARMOR || tmpl->category == ITEM_SHIELD) &&
               inst->ac_bonus != 0) {
        snprintf(suffix, sizeof(suffix), " [+%d]", inst->ac_bonus);
    } else if (tmpl->category == ITEM_LIGHT_SOURCE) {
        snprintf(suffix, sizeof(suffix), " (%d/%d light turns)", inst->durability,
                 tmpl->max_durability > 0 ? tmpl->max_durability : 100);
    } else if (tmpl->category == ITEM_FUEL) {
        snprintf(suffix, sizeof(suffix), " (%d light turns)",
                 tmpl->duration_turns > 0 ? tmpl->duration_turns : 300);
    }

    snprintf(buf, max_len, "%s%s%s", prefix, tmpl->name, suffix);
}

float get_current_weight(Client *c) {
    float w = (float)c->gold * 0.01f;
    ItemInstance *s[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                         &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                         &c->slot_arm_l,  &c->slot_feet};
    for (int i = 0; i < 10; i++) {
        if (s[i]->template_idx != -1) {
            w += item_database[s[i]->template_idx].weight;
        }
    }
    for (int i = 0; i < 10; i++) {
        if (c->slot_rings[i].template_idx != -1) {
            w += item_database[c->slot_rings[i].template_idx].weight;
        }
    }
    for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1) {
            w += item_database[c->belt[i].template_idx].weight * (float)c->belt[i].stack_count;
        }
    }
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx >= 0) {
            w += item_database[c->backpack[i].template_idx].weight *
                 (float)c->backpack[i].stack_count;
        }
    }
    return w;
}

float get_movement_cooldown(Client *c) {
    float w = get_current_weight(c);
    float limit = (float)c->str * 5.0f;
    if (w > limit * 2.0f) {
        return 1.0f;
    }
    if (w > limit) {
        return 0.4f;
    }
    return 0.2f;
}

int get_equipped_mask(Client *c) {
    int mask = 0;
    /*bits 0-9: single slots (head, neck, body, back, hand_r, hand_l,
   * hands, arm_r, arm_l, feet)
   * bits 10-19: slot_rings[0..9] (not overlapping with the previous ones)*/
    if (c->slot_head.template_idx != -1) {
        mask |= (1 << 0);
    }
    if (c->slot_neck.template_idx != -1) {
        mask |= (1 << 1);
    }
    if (c->slot_body.template_idx != -1) {
        mask |= (1 << 2);
    }
    if (c->slot_back.template_idx != -1) {
        mask |= (1 << 3);
    }
    if (c->slot_hand_r.template_idx != -1) {
        mask |= (1 << 4);
    }
    if (c->slot_hand_l.template_idx != -1) {
        mask |= (1 << 5);
    }
    if (c->slot_hands.template_idx != -1) {
        mask |= (1 << 6);
    }
    if (c->slot_arm_r.template_idx != -1) {
        mask |= (1 << 7);
    }
    if (c->slot_arm_l.template_idx != -1) {
        mask |= (1 << 8);
    }
    if (c->slot_feet.template_idx != -1) {
        mask |= (1 << 9);
    }
    /*Rings: bits 10..19 (no overlap with base slot)*/
    for (int i = 0; i < 10; i++) {
        if (c->slot_rings[i].template_idx != -1) {
            mask |= (1 << (10 + i));
        }
    }
    return mask;
}

int get_vision_radius(Client *c) {
    if (c->floor_id == 0) {
        return 50; //In the open city you can see very far
    }

    // --- BLINDNESS CONDITION ---
    if (rules_has_condition_t(c->effects, c->effect_count, COND_BLINDED)) {
        return 0; //If blinded, you see nothing (radius 0)
    }

    int best_radius = 4; //Minimal vision in the dark (very limited)

    //Check belt and hands for a light source (Torches, Lanterns)
    ItemInstance *slots[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                             &c->belt[1],     &c->belt[2],     &c->belt[3]};
    for (int i = 0; i < 6; i++) {
        if (slots[i]->template_idx != -1) {
            const ItemTemplate *it = &item_database[slots[i]->template_idx];
            if (it->category == ITEM_LIGHT_SOURCE && slots[i]->durability > 0) {
                if (it->light_radius > best_radius) {
                    best_radius = it->light_radius;
                }
            }
        }
    }
    return best_radius;
}

int get_player_ac(Client *c) {
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    int ac_b =
        (c->slot_body.template_idx != -1) ? item_database[c->slot_body.template_idx].ac_base : 0;
    int bonus = 0;
    if (c->slot_head.template_idx != -1) {
        bonus += item_database[c->slot_head.template_idx].ac_bonus;
    }
    if (c->slot_hand_r.template_idx != -1 &&
        item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD) {
        bonus += item_database[c->slot_hand_r.template_idx].ac_bonus;
    }
    if (c->slot_hand_l.template_idx != -1 &&
        item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD) {
        bonus += item_database[c->slot_hand_l.template_idx].ac_bonus;
    }
    if (c->slot_arm_r.template_idx != -1) {
        bonus += item_database[c->slot_arm_r.template_idx].ac_bonus;
    }
    if (c->slot_arm_l.template_idx != -1) {
        bonus += item_database[c->slot_arm_l.template_idx].ac_bonus;
    }
    int final_ac = (ac_b > 0 ? ac_b : 10 + rules_get_modifier(td)) + bonus;
    if (rules_has_condition_t(c->effects, c->effect_count, COND_PETRIFIED)) {
        final_ac += 10;
    }
    return final_ac + rules_event_bonus(c->effects, c->effect_count, EVENT_ON_DEFEND);
}

void damage_item(ItemInstance *it, int amt) {
    if (it->template_idx == -1 || item_database[it->template_idx].max_durability == 0) {
        return;
    }
    it->durability -= amt;
    if (it->durability < 0) {
        it->durability = 0;
    }
}

int get_xp_threshold(int level) {
    const int thresholds[] = {0,      300,    900,    2700,   6500,   14000,  23000,
                              34000,  48000,  64000,  85000,  100000, 120000, 140000,
                              165000, 195000, 225000, 265000, 305000, 355000};
    if (level < 1) {
        return 0;
    }
    if (level > 20) {
        return 355000;
    }
    return thresholds[level - 1];
}

void check_level_up(Client *c) {
    if (c->level >= 20) {
        return;
    }
    int next_threshold = get_xp_threshold(c->level + 1);
    bool leveled = false;

    while (c->xp >= next_threshold && c->level < 20) {
        c->level++;
        leveled = true;

        // d8 class hit dice approximation
        int hp_gain = rules_roll_dice(1, 8) + rules_get_modifier(c->con);
        if (hp_gain < 1) {
            hp_gain = 1;
        }
        c->max_hp += hp_gain;
        c->hp = c->max_hp; // Heal to full on level up

        //Magic/Ki: Unlock new spell slots based on class
        int highest_spell_lvl = 1;
        switch (c->class_id) {
        case CLASS_PALADIN:
        case CLASS_RANGER:
            highest_spell_lvl = c->level / 2;
            if (highest_spell_lvl > 5) {
                highest_spell_lvl = 5;
            }
            break;
        case CLASS_FIGHTER:
        case CLASS_MONK:
        case CLASS_BARBARIAN:
        case CLASS_ROGUE:
            /* Martial arts: all four classes unlock a new Ki tier every
       * 3 character levels, up to L6, so the six tomes of every
       * martial codex (sold at The Archive of a Thousand Battles)
       * are learnable AND castable. */
            highest_spell_lvl = (c->level + 2) / 3;
            if (highest_spell_lvl > 6) {
                highest_spell_lvl = 6;
            }
            break;
        default:
            highest_spell_lvl = (c->level + 1) / 2;
            if (highest_spell_lvl > MAX_SPELL_LEVEL) {
                highest_spell_lvl = MAX_SPELL_LEVEL;
            }
            break;
        }
        if (highest_spell_lvl < 1) {
            highest_spell_lvl = 1;
        }
        c->spell_slots_max[highest_spell_lvl]++;

        //Reset all slots
        for (int i = 1; i <= MAX_SPELL_LEVEL; i++) {
            c->spell_slots[i] = c->spell_slots_max[i];
        }

        c->unspent_stat_points += 3;

        send_text_to_client(c->sock, "[LEVEL] *** YOU HAVE RISEN TO LEVEL %d! ***", c->level);
        send_combat_event(c->sock, COMBAT_EV_LEVEL_UP, c->entity_id, c->level);
        send_text_to_client(c->sock,
                            "[LEVEL] Maximum HP increased by %d (Total: %d)."
                            "Health and mana restored.",
                            hp_gain, c->max_hp);
        send_text_to_client(
            c->sock,
            "[LEVEL] You got 3 stat points! Use 'train <stat>' to assign them (e.g. 'train str') or 'train random'.");

        if (c->class_id == CLASS_BARD || c->class_id == CLASS_CLERIC ||
            c->class_id == CLASS_DRUID || c->class_id == CLASS_PALADIN ||
            c->class_id == CLASS_RANGER || c->class_id == CLASS_SORCERER ||
            c->class_id == CLASS_WARLOCK || c->class_id == CLASS_WIZARD) {
            c->needs_study = true;
            send_text_to_client(
                c->sock,
                "[MAGIC] Your horizons expand. Head to the Temple to study your books and learn new spells!");
        } else if (c->class_id == CLASS_BARBARIAN || c->class_id == CLASS_FIGHTER ||
                   c->class_id == CLASS_MONK || c->class_id == CLASS_ROGUE) {
            c->needs_study = true;
            send_text_to_client(
                c->sock,
                "[KI] You have learned new techniques. Go to your Guild/Temple to study the manuals and master them!");
        }

        next_threshold = get_xp_threshold(c->level + 1);
    }

    if (leveled) {
        server_log("LEVELUP", "Player %s has reached level %d", c->username, c->level);
        save_player_data(c);
    }
}

/*The functions give_item_by_name and give_starting_gear remain here because
 * are used by the character creation logic in main_server.c.*/

static int get_item_idx_by_name(const char *name) {
    for (int i = 0; i < item_database_size; i++) {
        if (strcasecmp(item_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void give_item_by_name(Client *c, const char *name) {
    int idx = get_item_idx_by_name(name);
    if (idx == -1) {
        return;
    }
    if (c->backpack_count >= MAX_BACKPACK) {
        return;
    }

    ItemInstance it;
    memset(&it, 0, sizeof(ItemInstance));
    it.template_idx = idx;
    it.durability = (item_database[idx].category == ITEM_LIGHT_SOURCE &&
                     strcasestr(item_database[idx].name, "lantern"))
                        ? 5000
                        : item_database[idx].max_durability;
    it.stack_count = 1;
    it.is_identified = true;
    it.quality = QUALITY_NORMAL;
    it.blessing = BLESS_NORMAL;
    it.element = ELEM_NONE;

    c->backpack[c->backpack_count++] = it;
}

void give_starting_gear(Client *c) {
    switch (c->class_id) {
    case CLASS_BARBARIAN:
        give_item_by_name(c, "Greataxe");
        give_item_by_name(c, "Way of Fury");
        break;
    case CLASS_WIZARD:
        give_item_by_name(c, "Arcane Focus, Staff");
        give_item_by_name(c, "Book of the Arcane Novice");
        break;
    case CLASS_SORCERER:
        give_item_by_name(c, "Arcane Focus, Staff");
        give_item_by_name(c, "Awakening of the Arcane Blood");
        break;
    case CLASS_WARLOCK:
        give_item_by_name(c, "Arcane Focus, Staff");
        give_item_by_name(c, "Initiate's Pact");
        break;
    case CLASS_CLERIC:
        give_item_by_name(c, "Mace");
        give_item_by_name(c, "Shield");
        give_item_by_name(c, "Cleric's Missal");
        break;
    case CLASS_DRUID:
        give_item_by_name(c, "Quarterstaff");
        give_item_by_name(c, "Whispers of the Forest");
        break;
    case CLASS_BARD:
        give_item_by_name(c, "Rapier");
        give_item_by_name(c, "Ballads of the Caster");
        break;
    case CLASS_PALADIN:
        give_item_by_name(c, "Longsword");
        give_item_by_name(c, "Shield");
        give_item_by_name(c, "Chain Mail");
        give_item_by_name(c, "Vow of the Neophyte");
        break;
    case CLASS_RANGER:
        give_item_by_name(c, "Longbow");
        give_item_by_name(c, "Tracks of Nature");
        break;
    case CLASS_ROGUE:
        give_item_by_name(c, "Shortsword");
        give_item_by_name(c, "Dagger");
        give_item_by_name(c, "Cutpurse's Notes");
        break;
    case CLASS_FIGHTER:
        give_item_by_name(c, "Greatsword");
        give_item_by_name(c, "Chain Mail");
        give_item_by_name(c, "Recruit's Manual");
        break;
    case CLASS_MONK:
        give_item_by_name(c, "Quarterstaff");
        give_item_by_name(c, "Scroll of the Lotus");
        break;
    default:
        give_item_by_name(c, "Dagger");
        break;
    }

    /*-------------------------------------------------------
   * Grimoire initialization: assign cantrips (level 0)
   * and the 1st level spells of your caster class.
   * Non-spellcasting characters gain an empty grimoire
   * (known_spells = {0}) and will not be able to use 'cast'.
   * -------------------------------------------------------*/
    {
        /*Non-enchanter classes: empty grimoire (already cleared by calloc)*/
        static const bool IS_CASTER[CLASS_COUNT] = {
            [CLASS_BARBARIAN] = true, [CLASS_BARD] = true,    [CLASS_CLERIC] = true,
            [CLASS_DRUID] = true,     [CLASS_FIGHTER] = true, [CLASS_MONK] = true,
            [CLASS_PALADIN] = true,   [CLASS_RANGER] = true,  [CLASS_ROGUE] = true,
            [CLASS_SORCERER] = true,  [CLASS_WARLOCK] = true, [CLASS_WIZARD] = true,
        };
        if (IS_CASTER[c->class_id]) {
            uint32_t cls_bit = (1u << (int)c->class_id);
            for (int si = 0; si < spell_database_size; si++) {
                /*Learns cantrip (level 0) and 1st level spells of your class*/
                if (spell_database[si].level > 1) {
                    continue;
                }
                if (!(spell_database[si].class_mask & cls_bit)) {
                    continue;
                }
                /*Guarded: si may exceed MAX_SPELL_DB_SIZE on huge datasets*/
                spell_known_set(c, si);
            }
        }
    }
}
