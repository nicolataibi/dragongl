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
    drop_loot_from_monster(c, t);
    if (t->archetype == ARCH_BOSS) {
        handle_boss_death(game, c, t);
    }
    return xp_gained;
}

/*A bow shot spends an arrow of the belt; false after the message when
 * there is none.*/
static bool spend_arrow(Client *c) {
    for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 &&
            item_database[c->belt[i].template_idx].category == ITEM_AMMO) {
            c->belt[i].stack_count--;
            if (c->belt[i].stack_count <= 0) {
                c->belt[i].template_idx = -1;
            }
            return true;
        }
    }
    send_text_to_client(c->sock, "[ERROR] No arrows on belt!");
    return false;
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

/*Damage of a melee hit: the weapon's dice (1d4 unarmed, doubled on a
 * critical) + STR + the weapon's and the effects' bonuses + 1d6 for an
 * elemental weapon, at least 1; a frozen target takes double, a petrified
 * one half. Tells the player and returns the damage dealt.*/
static int melee_damage(Client *c, NPC *t, const ItemInstance *weapon, const ItemTemplate *w,
                        int str_score, bool is_crit) {
    bool armed = weapon->template_idx != -1;
    int dc = (w ? w->damage_dice_count : 1) * (is_crit ? 2 : 1);
    int ds = w ? w->damage_dice_sides : 4;
    int roll_d = rules_roll_dice(dc, ds);
    int mod_d = rules_get_modifier(str_score) + (armed ? weapon->to_dam_bonus : 0) +
                rules_event_bonus(c->effects, c->effect_count, EVENT_ON_DAMAGE);
    if (armed && weapon->is_artifact) {
        mod_d += weapon->artifact_str_bonus;
    }
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

static void announce_attack(Client *c, const NPC *t, const ItemTemplate *w) {
    if (w && w->category == ITEM_WEAPON && w->damage_dice_sides <= 6) {
        send_text_to_client(c->sock, "[ACTION] Quickly dash and launch an attack against %s!",
                            npc_name(t));
    } else if (w && w->category == ITEM_WEAPON) {
        send_text_to_client(c->sock, "[ACTION] You load the shot and lower your weapon on %s!",
                            npc_name(t));
    } else {
        send_text_to_client(c->sock, "[ACTION] You attack %s with your bare hands!", npc_name(t));
    }
}

/*A melee attack of a player: d20 + weapon + ability (DEX for weapons up
 * to d6, else STR) + half the level against AC. A hit on a paralyzed,
 * stunned or unconscious target is a critical; a blinded attacker has
 * disadvantage.*/
void perform_attack(Client *c, NPC *t, Game *game) {
    if (!c || !t || !client_can_act(c)) {
        return;
    }
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    /*Never NULL: one of the two hand slots is always selected.*/
    ItemInstance *weapon = (c->slot_hand_r.template_idx != -1) ? &c->slot_hand_r : &c->slot_hand_l;
    const ItemTemplate *w =
        (weapon->template_idx != -1) ? &item_database[weapon->template_idx] : NULL;
    if (w && strcmp(w->name, "Shortbow") == 0 && !spend_arrow(c)) {
        return;
    }
    int a_mod = (w && w->damage_dice_sides <= 6) ? rules_get_modifier(td) : rules_get_modifier(ts);
    RuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.type = EVENT_ON_ATTACK;
    ctx.base_value = (w ? w->attack_bonus + weapon->to_hit_bonus : 0) + a_mod + (c->level / 2);
    ctx.source_id = c->entity_id;
    ctx.target_id = t->entity_id;
    rules_apply_modifiers(&ctx, c->effects, c->effect_count);
    if (rules_has_condition_t(c->effects, c->effect_count, COND_BLINDED)) {
        ctx.has_disadvantage = true;
    }
    bool is_crit = false;
    int roll_v = 0;
    bool hit = rules_roll_attack_detailed(ctx.final_value, t->ac, ctx.has_advantage,
                                          ctx.has_disadvantage, &is_crit, &roll_v);
    if (hit && (rules_has_condition_t(t->effects, t->effect_count, COND_PARALYZED) ||
                rules_has_condition_t(t->effects, t->effect_count, COND_STUNNED) ||
                rules_has_condition_t(t->effects, t->effect_count, COND_UNCONSCIOUS))) {
        is_crit = true;
    }
    announce_attack(c, t, w);
    send_text_to_client(c->sock, "> Attack Roll: 1d20 [%d] %+d = %d (VS AC %d)%s", roll_v,
                        ctx.final_value, roll_v + ctx.final_value, t->ac,
                        is_crit ? " [CRITICAL!]" : "");
    alert_neighbours(c, t, game);
    if (!hit) {
        clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, t->ac, false, false, 0);
        send_text_to_client(c->sock,
                            "> Missed... the attack bounces off the armor of"
                            " %s, leaving not even a scratch.",
                            npc_name(t));
        send_combat_event(c->sock, COMBAT_EV_MISS_DEALT, t->entity_id, 0);
        return;
    }
    int d = melee_damage(c, t, weapon, w, ts, is_crit);
    send_combat_event(c->sock, is_crit ? COMBAT_EV_CRITICAL : COMBAT_EV_DAMAGE_DEALT, t->entity_id,
                      d);
    clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, t->ac, true, is_crit, d);
    /*Wear: 5% of the hits cost the weapon a point of durability, 3% the armor.*/
    wear_item(c, weapon, 5, "weapon");
    wear_item(c, &c->slot_body, 3, "armor");
    if (t->hp <= 0) {
        npc_killed(game, c, t, NULL);
    }
}

void perform_attack_npc(NPC *n, Client *c, Game *game) {
    if (!n || !c || !c->active || !c->authenticated) {
        return;
    }
    int p_ac = get_player_ac(c);

    bool is_crit = false;
    int n_bonus = n->attack_bonus;
    int roll_v = 0;

    //--- ATTACKER NPC CONDITIONS ---
    bool adv = false, dis = false;
    if (rules_has_condition_t(n->effects, n->effect_count, COND_FRIGHTENED)) {
        dis = true; //Disadvantage if scared
    }
    if (rules_has_condition_t(c->effects, c->effect_count, COND_INVISIBLE)) {
        dis = true; // Disadvantage if the target is invisible
    }
    if (rules_has_condition_t(n->effects, n->effect_count, COND_CHARMED)) {
        // If the NPC is charmed by the player, it does not attack
        return;
    }

    bool hit = rules_roll_attack_detailed(n_bonus, p_ac, adv, dis, &is_crit, &roll_v);

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
        if (c->slot_body.template_idx != -1) {
            damage_item(&c->slot_body, 1);
        }
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
    if (!boss || boss->archetype != ARCH_BOSS) {
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
