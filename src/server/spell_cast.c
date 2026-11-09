/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_cast.c - A spell cast in the world.
 *
 * The record of the spell (data/spells.json, "effect") says what it does;
 * this file finds who it reaches and applies it:
 *   - single: the nearest hostile creature within range;
 *   - circle / cloud with a range: centred on that creature; with range 0
 *     centred on the caster;
 *   - cone / line: from the caster towards the nearest creature within
 *     the length;
 *   - self: the caster (heal, buff).
 * Ranges and radii are in cells of 5 feet, measured as the larger of the
 * two axis distances (a diagonal step is one cell).
 */

#include "spell_cast.h"

#include "aoe.h"
#include "combat_log.h"
#include "server_combat.h"
#include "server_internal.h"
#include "server_net.h"
#include "server_world.h"
#include "spell_effect.h"
#include "spell_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spell_caster_of(Client *c, SpellCaster *out) {
    int s[ABILITY_COUNT];
    get_total_stats(c, &s[ABILITY_STR], &s[ABILITY_DEX], &s[ABILITY_CON], &s[ABILITY_INT],
                    &s[ABILITY_WIS], &s[ABILITY_CHA]);
    for (int a = 0; a < ABILITY_COUNT; a++) {
        out->mods[a] = rules_get_modifier(s[a]);
    }
    out->level = c->level > 0 ? c->level : 1;
    out->spell_ability = class_spell_ability(c->class_id);
}

int npc_save_bonus(const NPC *n) {
    int bonus = 2 + (n->floor_id > 0 ? n->floor_id / 10 : 0);
    if (bonus > 9) {
        bonus = 9;
    }
    return n->archetype == ARCH_BOSS ? bonus + 2 : bonus;
}

bool spell_can_affect_npc(const NPC *n) {
    if (!n->active || n->archetype == ARCH_MERCHANT || n->archetype == ARCH_TREASURE ||
        n->archetype == ARCH_GOLD) {
        return false;
    }
    return strncmp(n->custom_name, "[SUMMONED]", 10) != 0 &&
           strncmp(n->custom_name, "[DOMINATED]", 11) != 0;
}

static DamageModifier npc_damage_mod(const NPC *n, DamageType type) {
    if (n->template && type < MAX_DAMAGE_TYPES) {
        if (n->template->damage_immunities[type]) {
            return DMG_MOD_IMMUNITY;
        }
        bool res = n->template->damage_resistances[type];
        bool vul = n->template->damage_vulnerabilities[type];
        if (res != vul) {
            return res ? DMG_MOD_RESISTANCE : DMG_MOD_VULNERABILITY;
        }
    }
    if (rules_has_condition_t((ActiveEffect *)n->effects, n->effect_count, COND_FROZEN)) {
        return DMG_MOD_VULNERABILITY;
    }
    if (rules_has_condition_t((ActiveEffect *)n->effects, n->effect_count, COND_PETRIFIED)) {
        return DMG_MOD_RESISTANCE;
    }
    return DMG_MOD_NORMAL;
}

static bool npc_has(const NPC *n, ConditionType cond) {
    return rules_has_condition_t((ActiveEffect *)n->effects, n->effect_count, cond);
}

static SpellTargetInfo npc_target_info(const NPC *n, const SpellEffect *e, const Client *caster) {
    SpellTargetInfo t;
    memset(&t, 0, sizeof(t));
    t.ac = n->ac;
    t.save_bonus = npc_save_bonus(n);
    t.save_disadvantage = npc_has(n, COND_CURSED);
    t.attack_advantage = npc_has(n, COND_RESTRAINED) || npc_has(n, COND_PARALYZED) ||
                         npc_has(n, COND_STUNNED) || npc_has(n, COND_UNCONSCIOUS) ||
                         npc_has(n, COND_BLINDED);
    t.attack_disadvantage = caster && rules_has_condition_t((ActiveEffect *)caster->effects,
                                                            caster->effect_count, COND_BLINDED);
    t.damage_mod = npc_damage_mod(n, e->damage_type);
    t.condition_immune = e->condition < MAX_CONDITIONS && n->template &&
                         n->template->condition_immunities[e->condition];
    return t;
}

static void npc_take_condition(NPC *n, ConditionType cond, int rounds) {
    ActiveEffect fx;
    memset(&fx, 0, sizeof(fx));
    fx.name = condition_to_name(cond);
    fx.trigger = EVENT_ON_TURN_START;
    fx.duration_rounds = rounds;
    rules_add_effect(n->effects, &n->effect_count, MAX_EFFECTS_PER_ENTITY, &fx);
}

static const char *attack_word(const SpellOutcome *o) {
    if (o->crit) {
        return "CRITICAL";
    }
    return o->landed ? "hit" : "miss";
}

/*"  > Goblin: attack 14+5 vs AC 13, hit: 9 fire damage (HP 3/12), Prone 1 round"*/
static void report(Client *caster, const SpellTemplate *sp, const SpellCaster *sc, const NPC *n,
                   const SpellOutcome *o, int rounds) {
    const SpellEffect *e = &sp->effect;
    char line[256];
    int len = snprintf(line, sizeof(line), "  > %s:", npc_name(n));
    if (e->roll == SPELL_ROLL_ATTACK) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, " attack %d%+d vs AC %d, %s",
                        o->roll, spell_attack_bonus(sc), n->ac, attack_word(o));
    } else if (e->roll == SPELL_ROLL_SAVE) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, " save %d%+d vs DC %d, %s", o->roll,
                        npc_save_bonus(n), spell_save_dc(e, sc), o->saved ? "resisted" : "failed");
    }
    if (e->kind == SPELL_KIND_DAMAGE && (o->landed || o->damage > 0)) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, ": %d %s damage (HP %d/%d)",
                        o->damage, damage_type_to_name(e->damage_type), n->hp, n->max_hp);
    }
    if (o->condition) {
        snprintf(line + len, sizeof(line) - (size_t)len, ", %s %d round%s",
                 condition_to_name(e->condition), rounds, rounds == 1 ? "" : "s");
    } else if (o->landed && e->condition < MAX_CONDITIONS && e->roll != SPELL_ROLL_SAVE) {
        snprintf(line + len, sizeof(line) - (size_t)len, ", resists %s",
                 condition_to_name(e->condition));
    }
    send_text_to_client(caster->sock, "%s", line);
}

bool spell_affect_npc(const SpellTemplate *sp, const SpellCaster *sc, NPC *n, Client *caster,
                      int condition_rounds) {
    const SpellEffect *e = &sp->effect;
    SpellTargetInfo t = npc_target_info(n, e, caster);
    SpellOutcome o = spell_resolve(e, sc, &t);
    int rounds = condition_rounds > 0 ? condition_rounds : e->rounds;
    if (o.condition) {
        npc_take_condition(n, e->condition, rounds);
    }
    if (e->kind == SPELL_KIND_DAMAGE) {
        n->hp -= o.damage;
    }
    if (caster) {
        report(caster, sp, sc, n, &o, rounds);
        if (n->ai_ctx.current_target_id == -1) {
            n->ai_ctx.current_target_id = caster->entity_id;
        }
    }
    clog_spell(caster ? caster->username : sp->name, sp->name, npc_name(n), o.damage, o.saved);
    if (e->kind == SPELL_KIND_DAMAGE && n->hp <= 0 && n->active) {
        npc_killed(caster, n, sp->name);
    }
    return o.landed;
}

/* --- Aim ----------------------------------------------------------------- */

typedef struct {
    NPC *target;
    int ox, oy; /*origin: centre of a circle or cloud, apex of a cone or line*/
    int tx, ty; /*the target's cell (the direction of a cone or line)*/
} CastAim;

static int cells_between(int x1, int y1, int x2, int y2) {
    int dx = abs(x1 - x2);
    int dy = abs(y1 - y2);
    return dx > dy ? dx : dy;
}

static NPC *nearest_target(const Client *c, NPC *npcs, int reach) {
    NPC *best = NULL;
    int best_d = reach + 1;
    FloorIter walk = floor_iter(npcs, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        NPC *n = &npcs[i];
        if (n->floor_id != c->floor_id || !spell_can_affect_npc(n)) {
            continue;
        }
        int d = cells_between(c->x, c->y, n->x, n->y);
        if (d < best_d) {
            best_d = d;
            best = n;
        }
    }
    return best;
}

/*Effects that cannot be cast without a creature to aim at.*/
static bool needs_target(const SpellEffect *e) {
    switch (e->kind) {
    case SPELL_KIND_DAMAGE:
    case SPELL_KIND_CONDITION:
        return e->target == SPELL_TARGET_SINGLE || e->target == SPELL_TARGET_AOE_CONE ||
               e->target == SPELL_TARGET_AOE_LINE || e->range > 0;
    case SPELL_KIND_SPECIAL:
        return e->target == SPELL_TARGET_SINGLE;
    default:
        return false;
    }
}

static bool aim(const Client *c, const SpellTemplate *sp, NPC *npcs, CastAim *a) {
    const SpellEffect *e = &sp->effect;
    memset(a, 0, sizeof(*a));
    a->ox = a->tx = c->x;
    a->oy = a->ty = c->y;
    if (e->target == SPELL_TARGET_SELF) {
        return true;
    }
    bool from_caster = e->target == SPELL_TARGET_AOE_CONE || e->target == SPELL_TARGET_AOE_LINE;
    int reach = from_caster ? e->radius : e->range;
    a->target = reach > 0 ? nearest_target(c, npcs, reach) : NULL;
    if (!a->target) {
        if (needs_target(e)) {
            send_text_to_client(c->sock, "[MAGIC] %s: no enemy within %d cells.", sp->name, reach);
            return false;
        }
        return true;
    }
    a->tx = a->target->x;
    a->ty = a->target->y;
    if (!from_caster && e->range > 0) {
        a->ox = a->tx;
        a->oy = a->ty;
    }
    return true;
}

/* --- Effects --------------------------------------------------------------- */

static void cast_heal(Client *c, const SpellTemplate *sp, const SpellCaster *sc) {
    int amount = spell_roll_amount(&sp->effect, sc, false);
    int before = c->hp;
    c->hp = c->hp + amount > c->max_hp ? c->max_hp : c->hp + amount;
    send_text_to_client(c->sock, "[MAGIC] %s: you recover %d HP (HP %d/%d).", sp->name,
                        c->hp - before, c->hp, c->max_hp);
    send_combat_event(c->sock, COMBAT_EV_HEAL, c->entity_id, c->hp - before);
}

static void cast_buff(Client *c, const SpellTemplate *sp) {
    static const RuleEventType k_event[] = {
        [SPELL_BUFF_AC] = EVENT_ON_DEFEND,
        [SPELL_BUFF_ATTACK] = EVENT_ON_ATTACK,
        [SPELL_BUFF_DAMAGE] = EVENT_ON_DAMAGE,
    };
    static const char *const k_stat[] = {
        [SPELL_BUFF_AC] = "AC", [SPELL_BUFF_ATTACK] = "to attack", [SPELL_BUFF_DAMAGE] = "damage"};
    const SpellEffect *e = &sp->effect;
    ActiveEffect fx;
    memset(&fx, 0, sizeof(fx));
    fx.name = sp->name;
    fx.trigger = k_event[e->stat];
    fx.mod_type = MOD_ADDITIVE;
    fx.value = e->value;
    fx.duration_rounds = e->rounds;
    if (!rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &fx)) {
        send_text_to_client(c->sock, "[MAGIC] You cannot sustain another magical effect.");
        return;
    }
    send_text_to_client(c->sock, "[MAGIC] %s: %+d %s for %d rounds.", sp->name, e->value,
                        k_stat[e->stat], e->rounds);
    send_detailed_state(c);
}

static void cast_offense(Client *c, const SpellTemplate *sp, const SpellCaster *sc, NPC *npcs,
                         const CastAim *a) {
    const SpellEffect *e = &sp->effect;
    if (e->target == SPELL_TARGET_SINGLE) {
        send_text_to_client(c->sock, "[ACTION] You cast %s at %s!", sp->name, npc_name(a->target));
        spell_affect_npc(sp, sc, a->target, c, 0);
        return;
    }
    AoeArea area = {e->target, e->radius, a->ox, a->oy, a->tx, a->ty};
    aoe_announce(c, sp);
    if (e->kind == SPELL_KIND_DAMAGE) {
        aoe_terraform(&area, c->floor_id, e->damage_type);
    }
    int cloud_rounds = e->target == SPELL_TARGET_AOE_CLOUD ? 1 : 0;
    int hits = 0;
    FloorIter walk = floor_iter(npcs, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        NPC *n = &npcs[i];
        if (n->floor_id == c->floor_id && spell_can_affect_npc(n) &&
            aoe_contains(&area, n->x, n->y)) {
            spell_affect_npc(sp, sc, n, c, cloud_rounds);
            hits++;
        }
    }
    if (e->target == SPELL_TARGET_AOE_CLOUD &&
        !aoe_spawn_cloud(sp, sc, c->username, c->floor_id, a->ox, a->oy)) {
        send_text_to_client(c->sock, "  > The air is saturated: the cloud cannot linger.");
    }
    if (hits == 0) {
        send_text_to_client(c->sock, "  > No creature is caught in the area.");
    } else {
        send_text_to_client(c->sock, "  > %d creature%s caught in the area.", hits,
                            hits == 1 ? "" : "s");
    }
}

bool spell_cast(Client *c, const SpellTemplate *sp, NPC *npcs) {
    CastAim a;
    if (!aim(c, sp, npcs, &a)) {
        return false;
    }
    SpellCaster sc;
    spell_caster_of(c, &sc);
    broadcast_spell_vfx(c->x, c->y, a.tx, a.ty, sp->vfx_type, sp->vfx_r, sp->vfx_g, sp->vfx_b,
                        c->floor_id);
    switch (sp->effect.kind) {
    case SPELL_KIND_DAMAGE:
    case SPELL_KIND_CONDITION:
        cast_offense(c, sp, &sc, npcs, &a);
        break;
    case SPELL_KIND_HEAL:
        cast_heal(c, sp, &sc);
        break;
    case SPELL_KIND_BUFF:
        cast_buff(c, sp);
        break;
    case SPELL_KIND_SPECIAL: {
        SpellContext ctx = {c, sp, npcs, a.target};
        spell_router_dispatch(&ctx);
        break;
    }
    case SPELL_KIND_UTILITY:
    default:
        send_text_to_client(c->sock, "[MAGIC] You cast %s. (It has no effect in combat.)",
                            sp->name);
        break;
    }
    if (sp->effect.terrain != SPELL_TERRAIN_NONE) {
        spell_router_terrain(c, sp, a.ox, a.oy);
    }
    check_level_up(c);
    return true;
}
