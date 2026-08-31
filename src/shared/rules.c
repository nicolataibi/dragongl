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

void rules_apply_modifiers(RuleContext* ctx, ActiveEffect* effects, int effect_count) {
    ctx->final_value = ctx->base_value;
    ctx->has_advantage = false;
    ctx->has_disadvantage = false;

    for (int i = 0; i < effect_count; i++) {
        ActiveEffect* e = &effects[i];
        
        if (e->trigger != ctx->type) continue;
        if (e->duration_rounds <= 0 && !e->is_persistent) continue;

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

int rules_roll_dice(int count, int sides) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += (rand() % sides) + 1;
    }
    return total;
}

bool rules_update_effects(ActiveEffect* effects, int* effect_count) {
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
    return (score - 10) / 2;
}

int rules_roll_d20(bool advantage, bool disadvantage) {
    int roll1 = (rand() % 20) + 1;
    int roll2 = (rand() % 20) + 1;

    if (advantage && !disadvantage) {
        return (roll1 > roll2) ? roll1 : roll2;
    } else if (disadvantage && !advantage) {
        return (roll1 < roll2) ? roll1 : roll2;
    }
    
    return roll1; //If both or neither, normal roll
}

bool rules_roll_attack(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit) {
    return rules_roll_attack_detailed(bonus, ac, advantage, disadvantage, is_crit, NULL);
}

bool rules_roll_attack_detailed(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit, int *out_roll) {
    int roll = rules_roll_d20(advantage, disadvantage);
    if (out_roll) *out_roll = roll;
    
    if (is_crit) {
        *is_crit = false;
    }

    if (roll == 20) {
        if (is_crit) *is_crit = true;
        return true; //Critic always hits
    } else if (roll == 1) {
        return false; //Critical failure is always missing
    }

    return (roll + bonus) >= ac;
}

bool rules_roll_save(int modifier, int dc, bool advantage, bool disadvantage, int* out_roll) {
    int roll = rules_roll_d20(advantage, disadvantage);
    if (out_roll) *out_roll = roll;
    
    if (roll == 20) return true;
    if (roll == 1) return false;

    return (roll + modifier) >= dc;
}

bool rules_has_condition(ActiveEffect* effects, int effect_count, const char* condition_name) {
    for (int i = 0; i < effect_count; i++) {
        if (strcasecmp(effects[i].name, condition_name) == 0) {
            if (effects[i].duration_rounds > 0 || effects[i].is_persistent) {
                return true;
            }
        }
    }
    return false;
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
