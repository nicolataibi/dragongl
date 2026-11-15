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

#include "server_combat.h"
#include "inventory.h"
#include "server_internal.h"
#include "server_world.h"
#include "server.h"
#include "player.h"
#include "server_loot.h"

// Define LOOT_DROP_CHANCE here since it is used in perform_attack -> drop_loot_from_monster
// NOTE: canonical value is defined in server_internal.h — do NOT redefine here.

/*NULL-safe display name for messages/combat log: custom_name (ghosts,
 * summons, dominated) > template name > archetype fallback. NPCs with
 * template==NULL (gold piles, chests, summoned elementals) used to
 * crash the server here: perform_attack* dereferenced n->template->name.*/
const char *npc_name(const NPC *n) {
    if (!n) {
        return "???";
    }
    if (n->custom_name[0] != '\0') {
        return n->custom_name;
    }
    if (n->template && n->template->name) {
        return n->template->name;
    }
    if (n->archetype == ARCH_TREASURE) {
        return "Treasure Chest";
    }
    if (n->archetype == ARCH_GOLD) {
        return "Gold Pile";
    }
    if (n->archetype == ARCH_MERCHANT) {
        return "Merchant";
    }
    return "???";
}

/*Party share of a kill: the members of the killer's party on his floor.*/
static int party_members_on_floor(Game *game, const Client *c) {
    if (c->party_leader[0] == '\0') {
        return 0;
    }
    int members = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (game->clients[i].active &&
            strcmp(game->clients[i].party_leader, c->party_leader) == 0 &&
            game->clients[i].floor_id == c->floor_id) {
            members++;
        }
    }
    return members;
}

static void award_kill_xp(Game *game, Client *c, const NPC *t, int xp_gained, int party_members) {
    if (party_members <= 1) {
        c->xp += xp_gained;
        check_level_up(c);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *m = &game->clients[i];
        if (m->active && strcmp(m->party_leader, c->party_leader) == 0 &&
            m->floor_id == c->floor_id) {
            m->xp += xp_gained;
            check_level_up(m);
            if (m != c) {
                send_text_to_client(m->sock, "[PARTY] Receive %d XP for killing %s.", xp_gained,
                                    npc_name(t));
            }
        }
    }
}

bool client_can_act(Client *c) {
    static const ConditionType k_blocking[] = {COND_PARALYZED, COND_STUNNED, COND_UNCONSCIOUS,
                                               COND_PETRIFIED, COND_FROZEN,  COND_INCAPACITATED};
    for (size_t i = 0; i < sizeof(k_blocking) / sizeof(k_blocking[0]); i++) {
        if (rules_has_condition_t(c->effects, c->effect_count, k_blocking[i])) {
            send_text_to_client(c->sock, "[SYSTEM] You are %s and cannot act!",
                                condition_to_name(k_blocking[i]));
            return false;
        }
    }
    return true;
}

int npc_killed(Game *game, Client *c, NPC *t, const char *by) {
    npc_set_active(game, t, false); /*also updates the floor caches*/
    t->respawn_timer = RESPAWN_TICKS;
    if (t->floor_id >= 0 && t->floor_id < MAX_FLOORS && t->x >= 0 && t->x < MAP_WIDTH &&
        t->y >= 0 && t->y < MAP_HEIGHT) {
        game->world->floors[t->floor_id].entity_grid[t->y][t->x] = 0;
    }
    if (!c) {
        clog_death(npc_name(t), "the environment", t->floor_id);
        if (t->archetype == ARCH_BOSS) {
            handle_boss_death(game, NULL, t);
        }
        return 0;
    }
    int party_members = party_members_on_floor(game, c);
    int xp_gained = party_members > 1 ? t->xp_reward / party_members : t->xp_reward;
    award_kill_xp(game, c, t, xp_gained, party_members);
    if (by) {
        send_text_to_client(c->sock, "[VICTORY] %s falls to your %s! (+%d XP)", npc_name(t), by,
                            xp_gained);
    } else {
        send_text_to_client(c->sock, "[VICTORY] With a fatal blow, you take %s' life! (+%d XP)",
                            npc_name(t), xp_gained);
    }
    if (game->event.type == 1 && t->floor_id == game->event.floor_id && t->template &&
        strcasestr(t->template->name, "Skeleton")) {
        game->event.progress++;
        send_text_to_client(c->sock, "[EVENT] You destroyed a minion of darkness! Progress: %d/%d",
                            game->event.progress, game->event.goal);
    }
    clog_death(npc_name(t), c->username, t->floor_id);
    drop_loot_from_monster(game, c, t);
    if (t->archetype == ARCH_BOSS) {
        handle_boss_death(game, c, t);
    }
    return xp_gained;
}

/*Aggro group: the NPCs within 5 steps of the target that have no target
 * turn on the attacker. Walks the per-floor index, which may be one tick
 * old (the filters cover a stale entry).*/
static void alert_neighbours(const Client *c, const NPC *t, Game *game) {
    FloorIter walk = floor_iter(game, t->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (&game->npcs[i] == t || game->npcs[i].archetype == ARCH_MERCHANT) {
            continue;
        }
        int d = abs(game->npcs[i].x - t->x) + abs(game->npcs[i].y - t->y);
        if (d <= 5 && game->npcs[i].ai_ctx.current_target_id == -1) {
            game->npcs[i].ai_ctx.current_target_id = c->entity_id;
        }
    }
}

/*An item that reached 0 durability degrades to Rusty.*/
static void wear_item(Client *c, ItemInstance *it, int chance_pct, const char *what) {
    if (it->template_idx == -1 || it->is_artifact || rand() % 100 >= chance_pct) {
        return;
    }
    damage_item(it, 1);
    if (it->durability <= 0) {
        it->quality = QUALITY_RUSTY;
        send_text_to_client(c->sock,
                            "[WARNING] Your %s is worn out! It degraded to Rusty."
                            " Repair at the Blacksmith on floor 0.",
                            what);
    }
}

/*What the conditions of the attacker and of the target do to an attack
 * (LORE.md, status conditions): a blinded or frightened attacker has
 * disadvantage, an invisible one advantage; an invisible target imposes
 * disadvantage; a hit on a paralyzed, stunned or unconscious target is a
 * critical; a petrified creature has +10 AC (a character's AC counts it
 * already, see get_player_ac).*/
typedef struct {
    bool adv, dis, auto_crit;
    int ac_bonus;
} AttackSituation;

static void attack_situation(ActiveEffect *att, int n_att, ActiveEffect *tgt, int n_tgt,
                             bool target_is_npc, AttackSituation *s) {
    memset(s, 0, sizeof(*s));
    s->dis = rules_has_condition_t(att, n_att, COND_BLINDED) ||
             rules_has_condition_t(att, n_att, COND_FRIGHTENED) ||
             rules_has_condition_t(tgt, n_tgt, COND_INVISIBLE);
    s->adv = rules_has_condition_t(att, n_att, COND_INVISIBLE);
    s->auto_crit = rules_has_condition_t(tgt, n_tgt, COND_PARALYZED) ||
                   rules_has_condition_t(tgt, n_tgt, COND_STUNNED) ||
                   rules_has_condition_t(tgt, n_tgt, COND_UNCONSCIOUS);
    if (target_is_npc && rules_has_condition_t(tgt, n_tgt, COND_PETRIFIED)) {
        s->ac_bonus = 10;
    }
}

int item_quality_bonus(const ItemInstance *it) {
    if (it->template_idx < 0) {
        return 0;
    }
    switch (it->quality) {
    case QUALITY_RUSTY:
        return -1;
    case QUALITY_FINE:
        return 1;
    case QUALITY_MASTERWORK:
        return 2;
    default:
        return 0;
    }
}

/*A shot spends one unit of the weapon's ammunition from the belt; false
 * after the message when there is none.*/
static bool spend_ammo(Client *c, const ItemTemplate *w) {
    static const char *const k_names[] = {"ammunition", "arrows", "bolts", "sling bullets",
                                          "blowgun needles"};
    for (int i = 0; i < MAX_BELT; i++) {
        ItemInstance *it = &c->belt[i];
        if (it->template_idx != -1 && it->stack_count > 0 &&
            item_database[it->template_idx].category == ITEM_AMMO &&
            item_database[it->template_idx].ammo == w->ammo) {
            if (--it->stack_count <= 0) {
                it->template_idx = -1;
                it->stack_count = 0;
            }
            return true;
        }
    }
    send_text_to_client(c->sock, "[ERROR] No %s on your belt for the %s!", k_names[w->ammo],
                        w->name);
    return false;
}

/*The item the player strikes or shoots with: the right hand, else the
 * left one (never NULL; its template_idx is -1 bare-handed).*/
static ItemInstance *wielded(Client *c) {
    return (c->slot_hand_r.template_idx != -1) ? &c->slot_hand_r : &c->slot_hand_l;
}

/*The to-hit or to-damage bonus of what is worn besides the weapon: the
 * artifacts (gloves, rings, a cloak...) carry one.*/
static int worn_bonus(Client *c, bool damage) {
    int bonus = 0;
    for (int i = 0; i < EQUIP_SLOT_COUNT + 10; i++) {
        const ItemInstance *s =
            i < EQUIP_SLOT_COUNT ? equip_slot(c, i) : &c->slot_rings[i - EQUIP_SLOT_COUNT];
        if (s->template_idx == -1 || s == &c->slot_hand_r || s == &c->slot_hand_l) {
            continue;
        }
        bonus += damage ? s->to_dam_bonus : s->to_hit_bonus;
    }
    return bonus;
}

/*The ability of an attack: DEX for a ranged weapon, the better of STR and
 * DEX for a finesse one, STR otherwise (bare hands too).*/
static int attack_ability(const ItemTemplate *w, int str_score, int dex_score) {
    if (w && w->ammo != AMMO_NONE) {
        return dex_score;
    }
    if (w && w->finesse && dex_score > str_score) {
        return dex_score;
    }
    return str_score;
}

/*Damage of a hit: the weapon's dice (1d4 unarmed, doubled on a critical)
 * + the ability + the weapon's enchantment and quality + the worn bonuses
 * and the effects + 1d6 for an elemental weapon, at least 1; a frozen
 * target takes double, a petrified one half. Tells the player and returns
 * the damage dealt.*/
static int strike_damage(Client *c, NPC *t, const ItemInstance *weapon, const ItemTemplate *w,
                         int ability_score, bool is_crit) {
    bool armed = weapon->template_idx != -1;
    int dc = (w ? w->damage_dice_count : 1) * (is_crit ? 2 : 1);
    int ds = w ? w->damage_dice_sides : 4;
    int roll_d = rules_roll_dice(dc, ds);
    int mod_d = rules_get_modifier(ability_score) +
                (armed ? weapon->to_dam_bonus + item_quality_bonus(weapon) : 0) +
                worn_bonus(c, true) +
                rules_event_bonus(c->effects, c->effect_count, EVENT_ON_DAMAGE);
    int element_d = (armed && weapon->element != ELEM_NONE) ? rules_roll_dice(1, 6) : 0;
    int d = roll_d + mod_d + element_d;
    if (d < 1) {
        d = 1;
    }
    DamageModifier mod = DMG_MOD_NORMAL;
    if (rules_has_condition_t(t->effects, t->effect_count, COND_FROZEN)) {
        mod = DMG_MOD_VULNERABILITY;
    } else if (rules_has_condition_t(t->effects, t->effect_count, COND_PETRIFIED)) {
        mod = DMG_MOD_RESISTANCE;
    }
    d = rules_calculate_damage(d, mod);
    t->hp -= d;
    send_text_to_client(c->sock,
                        is_crit ? "> BLOOD! A devastating blow [%dd%d: %d %+d (+%d elem) = %d"
                                  " damage]! (HP: %d/%d)"
                                : "  > Hit! [%dd%d: %d %+d (+%d elem) = %d damage] (HP: %d/%d)",
                        dc, ds, roll_d, mod_d, element_d, d, t->hp, t->max_hp);
    return d;
}

static void announce_attack(Client *c, const NPC *t, const ItemTemplate *w, bool shot) {
    if (shot && w) {
        send_text_to_client(c->sock, "[ACTION] You shoot your %s at %s!", w->name, npc_name(t));
    } else if (w && w->category == ITEM_WEAPON && w->ammo != AMMO_NONE) {
        send_text_to_client(c->sock, "[ACTION] You shoot %s point-blank (disadvantage)!",
                            npc_name(t));
    } else if (w && w->category == ITEM_WEAPON) {
        send_text_to_client(c->sock, "[ACTION] Quickly dash and launch an attack against %s!",
                            npc_name(t));
    } else {
        send_text_to_client(c->sock, "[ACTION] You attack %s with your bare hands!", npc_name(t));
    }
}

/*An attack ends invisibility. Swap-remove: the last effect takes the
 * freed slot and is checked next, so the index advances only past an
 * effect that stays.*/
void client_reveal(Client *c) {
    int i = 0;
    while (i < c->effect_count) {
        if (condition_from_name(c->effects[i].name) == COND_INVISIBLE) {
            c->effects[i] = c->effects[--c->effect_count];
            send_text_to_client(c->sock, "[MAGIC] You strike: you are visible again.");
        } else {
            i++;
        }
    }
}

bool client_charmed_by(const Client *c, const NPC *n) {
    for (int i = 0; i < c->effect_count; i++) {
        if (condition_from_name(c->effects[i].name) == COND_CHARMED &&
            c->effects[i].value == n->entity_id) {
            return true;
        }
    }
    return false;
}

/*An attack of a player with what he wields: d20 + the ability + half the
 * level + the weapon's enchantment and quality + the worn bonuses against
 * AC, with the advantage and disadvantage of the conditions. A ranged
 * weapon spends its ammunition and uses DEX; shot at a neighbour (shot ==
 * false) it has disadvantage.*/
static void player_strike(Client *c, NPC *t, Game *game, bool shot) {
    if (!c || !t || !client_can_act(c)) {
        return;
    }
    if (client_charmed_by(c, t)) {
        send_text_to_client(c->sock, "[MAGIC] You are charmed: you cannot raise a hand against %s.",
                            npc_name(t));
        return;
    }
    ItemInstance *weapon = wielded(c);
    const ItemTemplate *w =
        (weapon->template_idx != -1) ? &item_database[weapon->template_idx] : NULL;
    bool ranged = w && w->category == ITEM_WEAPON && w->ammo != AMMO_NONE;
    if (ranged && !spend_ammo(c, w)) {
        return;
    }
    /*The situation before the attack reveals an invisible attacker.*/
    AttackSituation sit;
    attack_situation(c->effects, c->effect_count, t->effects, t->effect_count, true, &sit);
    client_reveal(c);
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    int ability = attack_ability(w, ts, td);
    RuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.type = EVENT_ON_ATTACK;
    ctx.base_value = (w ? w->attack_bonus + weapon->to_hit_bonus + item_quality_bonus(weapon) : 0) +
                     worn_bonus(c, false) + rules_get_modifier(ability) + (c->level / 2);
    ctx.source_id = c->entity_id;
    ctx.target_id = t->entity_id;
    rules_apply_modifiers(&ctx, c->effects, c->effect_count);
    bool adv = ctx.has_advantage || sit.adv;
    bool dis = ctx.has_disadvantage || sit.dis || (ranged && !shot);
    int ac = t->ac + sit.ac_bonus;
    bool is_crit = false;
    int roll_v = 0;
    bool hit = rules_roll_attack_detailed(ctx.final_value, ac, adv, dis, &is_crit, &roll_v);
    if (hit && sit.auto_crit) {
        is_crit = true;
    }
    announce_attack(c, t, w, shot);
    const char *roll_kind = "";
    if (adv && !dis) {
        roll_kind = " [ADVANTAGE]";
    } else if (dis && !adv) {
        roll_kind = " [DISADVANTAGE]";
    }
    send_text_to_client(c->sock, "> Attack Roll: 1d20 [%d] %+d = %d (VS AC %d)%s%s", roll_v,
                        ctx.final_value, roll_v + ctx.final_value, ac, roll_kind,
                        is_crit ? " [CRITICAL!]" : "");
    alert_neighbours(c, t, game);
    if (!hit) {
        clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, ac, false, false, 0);
        send_text_to_client(c->sock,
                            "> Missed... the attack bounces off the armor of"
                            " %s, leaving not even a scratch.",
                            npc_name(t));
        send_combat_event(c->sock, COMBAT_EV_MISS_DEALT, t->entity_id, 0);
        return;
    }
    int d = strike_damage(c, t, weapon, w, ability, is_crit);
    send_combat_event(c->sock, is_crit ? COMBAT_EV_CRITICAL : COMBAT_EV_DAMAGE_DEALT, t->entity_id,
                      d);
    clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, ac, true, is_crit, d);
    /*Wear: 5% of the hits cost the weapon a point of durability.*/
    wear_item(c, weapon, 5, "weapon");
    if (t->hp <= 0) {
        npc_killed(game, c, t, NULL);
    }
}

void perform_attack(Client *c, NPC *t, Game *game) {
    player_strike(c, t, game, false);
}

void perform_ranged_attack(Client *c, NPC *t, Game *game) {
    player_strike(c, t, game, true);
}

void perform_attack_npc(NPC *n, Client *c, Game *game) {
    if (!n || !c || !c->active || !c->authenticated) {
        return;
    }
    int p_ac = get_player_ac(c);

    bool is_crit = false;
    int n_bonus = n->attack_bonus;
    int roll_v = 0;

    /*A charmed NPC does not attack; the conditions of both sides give
     * advantage, disadvantage and the automatic critical.*/
    if (rules_has_condition_t(n->effects, n->effect_count, COND_CHARMED)) {
        return;
    }
    AttackSituation sit;
    attack_situation(n->effects, n->effect_count, c->effects, c->effect_count, false, &sit);

    bool hit = rules_roll_attack_detailed(n_bonus, p_ac, sit.adv, sit.dis, &is_crit, &roll_v);
    if (hit && sit.auto_crit) {
        is_crit = true;
    }

    //Attack text contextual to the archetype
    if (n->archetype == ARCH_ASSASSIN) {
        send_text_to_client(c->sock,
                            "[DANGER] %s slips into the shadows and attempts to "
                            "stab you: 1d20 [%d] %+d = %d (VS AC %d)%s",
                            npc_name(n), roll_v, n_bonus, roll_v + n_bonus, p_ac,
                            is_crit ? " [CRITICAL!]" : "");
    } else if (n->archetype == ARCH_DRAGON) {
        send_text_to_client(c->sock,
                            "[DANGER] %s roars and envelops you in flame: "
                            "1d20 [%d] %+d = %d (VS AC %d)%s",
                            npc_name(n), roll_v, n_bonus, roll_v + n_bonus, p_ac,
                            is_crit ? " [CRITICAL!]" : "");
    } else if (n->archetype == ARCH_BRUTE) {
        send_text_to_client(c->sock,
                            "[DANGER] %s raises his fists and delivers a "
                            "devastating blow: 1d20 [%d] %+d = %d (VS AC %d)%s",
                            npc_name(n), roll_v, n_bonus, roll_v + n_bonus, p_ac,
                            is_crit ? " [CRITICAL!]" : "");
    } else {
        send_text_to_client(c->sock, "[DANGER] %s attacks you: 1d20 [%d] %+d = %d (VS AC %d)%s",
                            npc_name(n), roll_v, n_bonus, roll_v + n_bonus, p_ac,
                            is_crit ? " [CRITICAL!]" : "");
    }
    //Aggro Group: Alert NPCs close to the attacker (per-floor index, see
    //perform_attack above for the rationale).
    FloorIter walk = floor_iter(game, n->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (&game->npcs[i] != n && game->npcs[i].archetype != ARCH_MERCHANT) {
            int d = abs(game->npcs[i].x - n->x) + abs(game->npcs[i].y - n->y);
            if (d <= 5 && game->npcs[i].ai_ctx.current_target_id == -1) {
                game->npcs[i].ai_ctx.current_target_id = c->entity_id;
            }
        }
    }

    if (hit) {
        int dc = n->damage_dice;
        if (is_crit) {
            dc *= 2;
        }
        int d = rules_roll_dice(dc, n->damage_sides);

        DamageModifier d_mod = DMG_MOD_NORMAL;
        if (rules_has_condition_t(c->effects, c->effect_count, COND_FROZEN)) {
            d_mod = DMG_MOD_VULNERABILITY;
        } else if (rules_has_condition_t(c->effects, c->effect_count, COND_PETRIFIED)) {
            d_mod = DMG_MOD_RESISTANCE;
        }

        d = rules_calculate_damage(d, d_mod);
        c->hp -= d;
        send_text_to_client(c->sock, "[COMBAT] You take %d damage!", d);
        send_combat_event(c->sock, COMBAT_EV_DAMAGE_TAKEN, c->entity_id, d);
        clog_attack(npc_name(n), c->username, roll_v, n_bonus, p_ac, true, is_crit, d);
        /*Wear: 3% of the hits received cost the armour a point.*/
        wear_item(c, &c->slot_body, 3, "armor");
        if (c->hp <= 0) {
            clog_death(c->username, npc_name(n), c->floor_id);
            save_bones(game, c);
            player_respawn_town(game, c);
            send_text_to_client(
                c->sock,
                "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
    } else {
        clog_attack(npc_name(n), c->username, roll_v, n_bonus, p_ac, false, false, 0);
        send_text_to_client(c->sock, "[COMBAT] %s missed you!", npc_name(n));
        send_combat_event(c->sock, COMBAT_EV_MISS_TAKEN, c->entity_id, 0);
    }
}

/*Design note (§2.6): boss rewards (gold + bosses_defeated flag + stairs
 * unlock) are SHARED by every player present on the floor, not scaled to
 * individual contribution. This is an intentional "shared progression"
 * decision for the co-op LAN setting (a boss floor is a party gate), but
 * it is documented here on purpose: change it if per-player rewards are
 * ever wanted.*/
/*DESIGN NOTE: the reward is SHARED floor-wide, on purpose. Every active
 * client on the boss' floor gets the gold and the bosses_defeated flag
 * (the stairs stay sealed for the whole floor until the boss is down,
 * so "defeating the boss" is a floor-level gate, not a personal one —
 * rewarding the whole floor keeps party play consistent: anyone who
 * fought next to the killer also benefits). The killer argument is
 * therefore intentionally unused; if this ever becomes a solo-quest
 * game mode, scale the reward to the contributor here.*/
void handle_boss_death(Game *game, Client *c, NPC *boss) {
    (void)c;
    /*A ghost (save_bones) fights as a boss but is not the boss of its
     * floor: killing it opens nothing and pays nothing.*/
    if (!boss || boss->archetype != ARCH_BOSS || boss->is_ghost) {
        return;
    }
    if (boss->floor_id > 0 && boss->floor_id % 10 == 0) {
        int boss_index = (boss->floor_id / 10) - 1;
        if (boss_index >= 0 && boss_index < 10) {
            uint64_t gold_reward = (uint64_t)boss->floor_id * 1000U;
            const char *boss_name = "Boss";
            if (boss->custom_name[0] != '\0') {
                boss_name = boss->custom_name;
            } else if (boss->template) {
                boss_name = boss->template->name;
            }
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (game->clients[i].active && game->clients[i].floor_id == boss->floor_id) {
                    game->clients[i].gold += gold_reward;
                    game->clients[i].bosses_defeated |= (1u << boss_index);
                    send_text_to_client(game->clients[i].sock,
                                        "[HEROIC QUEST] Boss %s has been defeated!"
                                        "You have obtained %lu gold and the stairs have unlocked!",
                                        boss_name, gold_reward);
                }
            }
        }
    }
}

/*The create_tombstone function has been moved to tombstone.c
 * like tombstone_create(Client *c).*/
