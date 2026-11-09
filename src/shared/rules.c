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

#include "rules.h"
#include <stdlib.h>
#include <time.h>
#include <strings.h>

void rules_apply_modifiers(RuleContext *ctx, ActiveEffect *effects, int effect_count) {
    ctx->final_value = ctx->base_value;
    ctx->has_advantage = false;
    ctx->has_disadvantage = false;

    for (int i = 0; i < effect_count; i++) {
        ActiveEffect *e = &effects[i];

        if (e->trigger != ctx->type) {
            continue;
        }
        if (e->duration_rounds <= 0 && !e->is_persistent) {
            continue;
        }

        switch (e->mod_type) {
        case MOD_ADDITIVE:
            ctx->final_value += e->value;
            break;
        case MOD_MULTIPLIER:
            ctx->final_value *= e->value;
            break;
        case MOD_ADVANTAGE:
            ctx->has_advantage = true;
            break;
        case MOD_DISADVANTAGE:
            ctx->has_disadvantage = true;
            break;
        }
    }
}

int rules_event_bonus(const ActiveEffect *effects, int effect_count, RuleEventType type) {
    int bonus = 0;
    for (int i = 0; i < effect_count; i++) {
        const ActiveEffect *e = &effects[i];
        if (e->trigger == type && e->mod_type == MOD_ADDITIVE &&
            (e->duration_rounds > 0 || e->is_persistent)) {
            bonus += e->value;
        }
    }
    return bonus;
}

bool rules_add_effect(ActiveEffect *effects, int *effect_count, int max_effects,
                      const ActiveEffect *effect) {
    for (int i = 0; i < *effect_count; i++) {
        ActiveEffect *e = &effects[i];
        if (e->name && effect->name && strcasecmp(e->name, effect->name) == 0) {
            if (effect->duration_rounds > e->duration_rounds) {
                e->duration_rounds = effect->duration_rounds;
            }
            if (effect->value > e->value) {
                e->value = effect->value;
            }
            e->is_persistent = e->is_persistent || effect->is_persistent;
            return true;
        }
    }
    if (*effect_count >= max_effects) {
        return false;
    }
    effects[(*effect_count)++] = *effect;
    return true;
}

int rules_roll_dice(int count, int sides) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += (rand() % sides) + 1;
    }
    return total;
}

bool rules_update_effects(ActiveEffect *effects, int *effect_count) {
    bool changed = false;
    for (int i = 0; i < *effect_count; i++) {
        if (!effects[i].is_persistent) {
            effects[i].duration_rounds--;
            if (effects[i].duration_rounds <= 0) {
                //Remove the effect by moving the last one into place
                effects[i] = effects[*effect_count - 1];
                (*effect_count)--;
                i--; // Recheck current index
                changed = true;
            }
        }
    }
    return changed;
}

int rules_get_modifier(int score) {
    /*DnD: floor((score-10)/2). Plain C division truncates TOWARD ZERO,
     * which is wrong for odd sub-10 scores (score 3: (3-10)/2 = -3
     * instead of -4; score 5: -2 instead of -3).*/
    int diff = score - 10;
    int q = diff / 2;
    if (diff < 0 && (diff & 1)) {
        q--;
    }
    return q;
}

static int g_forced_d20; /*0: real rolls (rules_force_d20)*/

void rules_force_d20(int value) {
    g_forced_d20 = (value >= 1 && value <= 20) ? value : 0;
}

int rules_roll_d20(bool advantage, bool disadvantage) {
    if (g_forced_d20) {
        return g_forced_d20;
    }
    int roll1 = (rand() % 20) + 1;
    int roll2 = (rand() % 20) + 1;

    if (advantage && !disadvantage) {
        return (roll1 > roll2) ? roll1 : roll2;
    }
    if (disadvantage && !advantage) {
        return (roll1 < roll2) ? roll1 : roll2;
    }

    return roll1; //If both or neither, normal roll
}

bool rules_roll_attack(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit) {
    return rules_roll_attack_detailed(bonus, ac, advantage, disadvantage, is_crit, NULL);
}

bool rules_roll_attack_detailed(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit,
                                int *out_roll) {
    int roll = rules_roll_d20(advantage, disadvantage);
    if (out_roll) {
        *out_roll = roll;
    }

    if (is_crit) {
        *is_crit = false;
    }

    if (roll == 20) {
        if (is_crit) {
            *is_crit = true;
        }
        return true; //Critic always hits
    }
    if (roll == 1) {
        return false; //Critical failure is always missing
    }

    return (roll + bonus) >= ac;
}

bool rules_roll_save(int modifier, int dc, bool advantage, bool disadvantage, int *out_roll) {
    int roll = rules_roll_d20(advantage, disadvantage);
    if (out_roll) {
        *out_roll = roll;
    }

    if (roll == 20) {
        return true;
    }
    if (roll == 1) {
        return false;
    }

    return (roll + modifier) >= dc;
}

/*Canonical display names of ConditionType — the SINGLE source of truth
 * for the strings that circulate as ActiveEffect.name. Renaming an
 * effect is a one-line change here, and every consumer keeps working
 * because all comparisons go through the table (condition_from_name /
 * rules_has_condition_t), never through ad-hoc literals.*/
static const char *const g_condition_names[MAX_CONDITIONS] = {
    [COND_BLINDED] = "Blinded",     [COND_CHARMED] = "Charmed",
    [COND_DEAFENED] = "Deafened",   [COND_FRIGHTENED] = "Frightened",
    [COND_GRAPPLED] = "Grappled",   [COND_INCAPACITATED] = "Incapacitated",
    [COND_INVISIBLE] = "Invisible", [COND_PARALYZED] = "Paralyzed",
    [COND_PETRIFIED] = "Petrified", [COND_POISONED] = "Poisoned",
    [COND_PRONE] = "Prone",         [COND_RESTRAINED] = "Restrained",
    [COND_STUNNED] = "Stunned",     [COND_UNCONSCIOUS] = "Unconscious",
    [COND_BURNING] = "Burning",     [COND_BLEEDING] = "Bleeding",
    [COND_CURSED] = "Cursed",       [COND_FROZEN] = "Frozen",
    [COND_SILENCED] = "Silenced",
};

const char *condition_to_name(ConditionType cond) {
    if ((int)cond < 0 || cond >= MAX_CONDITIONS) {
        return "Unknown";
    }
    return g_condition_names[cond];
}

ConditionType condition_from_name(const char *condition_name) {
    if (!condition_name) {
        return MAX_CONDITIONS;
    }
    for (int c = 0; c < MAX_CONDITIONS; c++) {
        if (strcasecmp(condition_name, g_condition_names[c]) == 0) {
            return (ConditionType)c;
        }
    }
    return MAX_CONDITIONS; /*not a condition (or unknown name)*/
}

bool rules_has_condition_t(ActiveEffect *effects, int effect_count, ConditionType cond) {
    if ((int)cond < 0 || cond >= MAX_CONDITIONS) {
        return false;
    }
    const char *name = g_condition_names[cond];
    for (int i = 0; i < effect_count; i++) {
        if (strcasecmp(effects[i].name, name) == 0) {
            if (effects[i].duration_rounds > 0 || effects[i].is_persistent) {
                return true;
            }
        }
    }
    return false;
}

bool rules_has_condition(ActiveEffect *effects, int effect_count, const char *condition_name) {
    /*String form routed through the canonical table, so it can never
     * disagree with the enum form (kept for display/log sites).*/
    return rules_has_condition_t(effects, effect_count, condition_from_name(condition_name));
}

int rules_calculate_damage(int raw_damage, DamageModifier dmg_mod) {
    switch (dmg_mod) {
    case DMG_MOD_IMMUNITY:
        return 0;
    case DMG_MOD_RESISTANCE:
        return raw_damage / 2;
    case DMG_MOD_VULNERABILITY:
        return raw_damage * 2;
    case DMG_MOD_NORMAL:
    default:
        return raw_damage;
    }
}

static const char *const g_damage_type_names[MAX_DAMAGE_TYPES] = {
    [DMG_SLASHING] = "slashing",   [DMG_PIERCING] = "piercing", [DMG_BLUDGEONING] = "bludgeoning",
    [DMG_POISON] = "poison",       [DMG_ACID] = "acid",         [DMG_FIRE] = "fire",
    [DMG_COLD] = "cold",           [DMG_RADIANT] = "radiant",   [DMG_NECROTIC] = "necrotic",
    [DMG_LIGHTNING] = "lightning", [DMG_THUNDER] = "thunder",   [DMG_FORCE] = "force",
    [DMG_PSYCHIC] = "psychic",
};

const char *damage_type_to_name(DamageType type) {
    if ((int)type < 0 || type >= MAX_DAMAGE_TYPES) {
        return "unknown";
    }
    return g_damage_type_names[type];
}

DamageType damage_type_from_name(const char *name) {
    for (int t = 0; name && t < MAX_DAMAGE_TYPES; t++) {
        if (strcasecmp(name, g_damage_type_names[t]) == 0) {
            return (DamageType)t;
        }
    }
    return MAX_DAMAGE_TYPES;
}

static const char *const g_ability_names[] = {"str", "dex", "con", "int", "wis", "cha", "spell"};

const char *ability_to_name(Ability a) {
    if ((int)a < 0 || a > ABILITY_SPELL) {
        return "none";
    }
    return g_ability_names[a];
}

Ability ability_from_name(const char *name) {
    for (int a = 0; name && a <= ABILITY_SPELL; a++) {
        if (strcasecmp(name, g_ability_names[a]) == 0) {
            return (Ability)a;
        }
    }
    return ABILITY_NONE;
}
