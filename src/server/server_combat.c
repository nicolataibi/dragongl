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


// Define LOOT_DROP_CHANCE here since it is used in perform_attack -> drop_loot_from_monster
// NOTE: canonical value is defined in server_internal.h — do NOT redefine here.

/*NULL-safe display name for messages/combat log: custom_name (ghosts,
 * summons, dominated) > template name > archetype fallback. NPCs with
 * template==NULL (gold piles, chests, summoned elementals) used to
 * crash the server here: perform_attack* dereferenced n->template->name.*/
const char *npc_name(const NPC *n) {
  if (!n)
    return "???";
  if (n->custom_name[0] != '\0')
    return n->custom_name;
  if (n->template && n->template->name)
    return n->template->name;
  if (n->archetype == ARCH_TREASURE)
    return "Treasure Chest";
  if (n->archetype == ARCH_GOLD)
    return "Gold Pile";
  if (n->archetype == ARCH_MERCHANT)
    return "Merchant";
  return "???";
}

void perform_attack(Client *c, NPC *t, NPC *npcs) {
  if (!c || !t)
    return;
  int ts, td, tc, ti, tw, th;
  get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
  ItemInstance *weapon =
      (c->slot_hand_r.template_idx != -1) ? &c->slot_hand_r : &c->slot_hand_l;
  const ItemTemplate *w = (weapon->template_idx != -1)
                              ? &item_database[weapon->template_idx]
                              : NULL;
  if (w && (strcmp(w->name, "Arco Corto") == 0 ||
            strcmp(w->name, "Shortbow") == 0)) {
    bool has_ammo = false;
    for (int i = 0; i < MAX_BELT; i++)
      if (c->belt[i].template_idx != -1 &&
          item_database[c->belt[i].template_idx].category == ITEM_AMMO) {
        c->belt[i].stack_count--;
        if (c->belt[i].stack_count <= 0)
          c->belt[i].template_idx = -1;
        has_ammo = true;
        break;
      }
    if (!has_ammo) {
      send_text_to_client(c->sock, "[ERROR] No arrows on belt!");
      return;
    }
  }
  int a_mod = (w && w->damage_dice_sides <= 6) ? rules_get_modifier(td)
                                               : rules_get_modifier(ts);
  int inst_to_hit = (weapon && weapon->template_idx != -1) ? weapon->to_hit_bonus : 0;
  RuleContext ctx;
  ctx.type = EVENT_ON_ATTACK;
  ctx.base_value = (w ? w->attack_bonus : 0) + inst_to_hit + a_mod + (c->level / 2);
  ctx.source_id = c->entity_id;
  ctx.target_id = t->entity_id;
  rules_apply_modifiers(&ctx, c->effects, c->effect_count);

  //--- ATTACKER CONDITIONS ---
  if (rules_has_condition_t(c->effects, c->effect_count, COND_BLINDED)) {
    ctx.has_disadvantage = true;
  }

  bool is_crit = false;
  int roll_v = 0;
  bool hit =
      rules_roll_attack_detailed(ctx.final_value, t->ac, ctx.has_advantage,
                                 ctx.has_disadvantage, &is_crit, &roll_v);

  //--- TARGET CONDITIONS ---
  if (hit &&
      (rules_has_condition_t(t->effects, t->effect_count, COND_PARALYZED) ||
       rules_has_condition_t(t->effects, t->effect_count, COND_STUNNED) ||
       rules_has_condition_t(t->effects, t->effect_count, COND_UNCONSCIOUS))) {
    is_crit = true; //Automatic critical hit against incapacitated targets
  }
  if (w && w->category == ITEM_WEAPON && w->damage_dice_sides <= 6) {
    send_text_to_client(c->sock,
                        "[ACTION] Quickly dash and launch an attack against %s!",
                        npc_name(t));
  } else if (w && w->category == ITEM_WEAPON) {
    send_text_to_client(c->sock,
                        "[ACTION] You load the shot and lower your weapon on %s!",
                        npc_name(t));
  } else {
    send_text_to_client(c->sock, "[ACTION] You attack %s with your bare hands!",
                        npc_name(t));
  }

  send_text_to_client(c->sock,
                      "> Attack Roll: 1d20 [%d] %+d = %d (VS AC %d)%s",
                      roll_v, ctx.final_value, roll_v + ctx.final_value, t->ac,
                      is_crit ? " [CRITICAL!]" : "");
  //Aggro Group: Alert NPCs near the target.
  //Walks the per-floor index (O(entities on the floor)); the stale-entry
  //filters below also cover an index that is up to one tick old.
  int aggro_n = 0;
  const int *aggro_idx = floor_index_for(t->floor_id, &aggro_n);
  for (int k = 0; k < ((aggro_idx != NULL) ? aggro_n : MAX_NPCS); k++) {
    int i = (aggro_idx != NULL) ? aggro_idx[k] : k;
    if (&npcs[i] != t && npcs[i].active && npcs[i].archetype != ARCH_MERCHANT &&
        npcs[i].floor_id == t->floor_id) {
      int d = abs(npcs[i].x - t->x) + abs(npcs[i].y - t->y);
      if (d <= 5 && npcs[i].ai_ctx.current_target_id == -1) {
        npcs[i].ai_ctx.current_target_id = c->entity_id;
      }
    }
  }

  if (hit) {
    int dc = w ? w->damage_dice_count : 1, ds = w ? w->damage_dice_sides : 4;
    if (is_crit)
      dc *= 2; //Double the dice on a critic
    int roll_d = rules_roll_dice(dc, ds);
    int inst_dam = (weapon && weapon->template_idx != -1) ? weapon->to_dam_bonus : 0;
    //Artifact: Add flat stat bonuses to damage
    if (weapon && weapon->template_idx != -1 && weapon->is_artifact)
      inst_dam += weapon->artifact_str_bonus;
    int mod_d = rules_get_modifier(ts) + inst_dam;
    int element_d = 0;
    if (weapon && weapon->template_idx != -1 && weapon->element != ELEM_NONE) {
        element_d = rules_roll_dice(1, 6);
    }
    int d = roll_d + mod_d + element_d;
    if (d < 1)
      d = 1;

    DamageModifier mod = DMG_MOD_NORMAL;
    //--- TARGET STATUS EFFECTS ON DAMAGE ---
    if (rules_has_condition_t(t->effects, t->effect_count, COND_FROZEN)) {
      mod = DMG_MOD_VULNERABILITY;
    } else if (rules_has_condition_t(t->effects, t->effect_count, COND_PETRIFIED)) {
      mod = DMG_MOD_RESISTANCE;
    }

    d = rules_calculate_damage(d, mod);
    t->hp -= d;
    if (is_crit) {
      send_text_to_client(c->sock,
                          "> BLOOD! A devastating blow [%dd%d: %d %+d (+%d elem) = %d"
                          "danni]! (HP: %d/%d)",
                          dc, ds, roll_d, mod_d, element_d, d, t->hp, t->max_hp);
    } else {
      send_text_to_client(c->sock,
                          "  > Colpito! [%dd%d: %d %+d (+%d elem) = %d danni] (HP: %d/%d)",
                          dc, ds, roll_d, mod_d, element_d, d, t->hp, t->max_hp);
    }
    clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, t->ac,
                true, is_crit, d);
    //===== COMBAT WEAR =====
    //5% chance per hit: weapon loses 1 durability
    if (weapon->template_idx != -1 && !weapon->is_artifact) {
      if (rand() % 100 < 5) {
        damage_item(weapon, 1);
        if (weapon->durability <= 0) {
          weapon->quality = QUALITY_RUSTY;
          send_text_to_client(c->sock,
              "[WARNING] Your weapon is worn out! She degraded herself to Rusty."
              " Repair at the Blacksmith on floor 0.");
        }
      }
    }
    //3% chance: armor loses 1 durability
    ItemInstance *armor = &c->slot_body;
    if (armor->template_idx != -1 && !armor->is_artifact && rand() % 100 < 3) {
      damage_item(armor, 1);
      if (armor->durability <= 0) {
        armor->quality = QUALITY_RUSTY;
        send_text_to_client(c->sock,
            "[WARNING] Your armor is worn out! She degraded herself to Rusty."
            " Repair at the Blacksmith on floor 0.");
      }
    }
    if (t->hp <= 0) {
      npc_set_active(t, false); /*also updates the O(1) floor cache (A2)*/
      t->respawn_timer = RESPAWN_TICKS;
      extern World *master_world;
      master_world->floors[t->floor_id].entity_grid[t->y][t->x] = 0;
      int party_members = 0;
      if (strlen(c->party_leader) > 0) {
          for (int i = 0; i < MAX_CLIENTS; i++) {
              if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0 && g_clients[i].floor_id == c->floor_id) {
                  party_members++;
              }
          }
      }
      
      int xp_gained = t->xp_reward;
      if (party_members > 1) {
          xp_gained = t->xp_reward / party_members;
          for (int i = 0; i < MAX_CLIENTS; i++) {
              if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0 && g_clients[i].floor_id == c->floor_id) {
                  g_clients[i].xp += xp_gained;
                  check_level_up(&g_clients[i]);
                  if (&g_clients[i] != c) {
                      send_text_to_client(g_clients[i].sock, "[PARTY] Receive %d XP for killing %s.", xp_gained, npc_name(t));
                  }
              }
          }
      } else {
          c->xp += xp_gained;
          check_level_up(c);
      }

      if (active_event_type == 1 && t->floor_id == event_floor_id && t->template) {
          if (strcasestr(t->template->name, "Skeleton") || strcasestr(t->template->name, "Scheletro")) {
              event_progress++;
              send_text_to_client(c->sock, "[EVENT] You destroyed a minion of darkness! Progress: %d/%d", event_progress, event_goal);
          }
      }

      /*The floor-stats update now happens inside npc_set_active(t, false)
       *above — the separate floor_stats_npc_died() call lived here and is
       *gone (single point of truth, A2).*/

      send_text_to_client(
          c->sock,
          "[VICTORY] With a fatal blow, you take %s' life! (+%d XP)",
          npc_name(t), xp_gained);
      clog_death(npc_name(t), c->username, t->floor_id);
      drop_loot_from_monster(c, t);
      if (t->archetype == ARCH_BOSS) {
        handle_boss_death(c, t);
      }
    }
  } else {
    clog_attack(c->username, npc_name(t), roll_v, ctx.final_value, t->ac,
                false, false, 0);
    send_text_to_client(c->sock,
                        "> Missed... the attack bounces off the armor of"
                        " %s, leaving not even a scratch.",
                        npc_name(t));
  }
}

void perform_attack_npc(NPC *n, Client *c, NPC *npcs) {
  if (!n || !c || !c->active || !c->authenticated)
    return;
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

  bool hit =
      rules_roll_attack_detailed(n_bonus, p_ac, adv, dis, &is_crit, &roll_v);

  //Attack text contextual to the archetype
  if (n->archetype == ARCH_ASSASSIN) {
    send_text_to_client(c->sock,
                        "[DANGER] %s slips into the shadows and attempts to "
                        "stab you: 1d20 [%d] %+d = %d (VS AC %d)%s",
                        npc_name(n), roll_v, n_bonus, roll_v + n_bonus,
                        p_ac, is_crit ? " [CRITICAL!]" : "");
  } else if (n->archetype == ARCH_DRAGON) {
    send_text_to_client(c->sock,
                        "[DANGER] %s roars and envelops you in flame: "
                        "1d20 [%d] %+d = %d (VS AC %d)%s",
                        npc_name(n), roll_v, n_bonus, roll_v + n_bonus,
                        p_ac, is_crit ? " [CRITICAL!]" : "");
  } else if (n->archetype == ARCH_BRUTE) {
    send_text_to_client(c->sock,
                        "[DANGER] %s raises his fists and delivers a "
                        "devastating blow: 1d20 [%d] %+d = %d (VS AC %d)%s",
                        npc_name(n), roll_v, n_bonus, roll_v + n_bonus,
                        p_ac, is_crit ? " [CRITICAL!]" : "");
  } else {
    send_text_to_client(
        c->sock, "[DANGER] %s attacks you: 1d20 [%d] %+d = %d (VS AC %d)%s",
        npc_name(n), roll_v, n_bonus, roll_v + n_bonus, p_ac,
        is_crit ? " [CRITICAL!]" : "");
  }
  //Aggro Group: Alert NPCs close to the attacker (per-floor index, see
  //perform_attack above for the rationale).
  int aggro_n = 0;
  const int *aggro_idx = floor_index_for(n->floor_id, &aggro_n);
  for (int k = 0; k < ((aggro_idx != NULL) ? aggro_n : MAX_NPCS); k++) {
    int i = (aggro_idx != NULL) ? aggro_idx[k] : k;
    if (&npcs[i] != n && npcs[i].active && npcs[i].archetype != ARCH_MERCHANT &&
        npcs[i].floor_id == n->floor_id) {
      int d = abs(npcs[i].x - n->x) + abs(npcs[i].y - n->y);
      if (d <= 5 && npcs[i].ai_ctx.current_target_id == -1) {
        npcs[i].ai_ctx.current_target_id = c->entity_id;
      }
    }
  }

  if (hit) {
    int dc = n->damage_dice;
    if (is_crit)
      dc *= 2;
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
    clog_attack(npc_name(n), c->username, roll_v, n_bonus, p_ac, true,
                is_crit, d);
    if (c->slot_body.template_idx != -1) {
      damage_item(&c->slot_body, 1);
    }
    if (c->hp <= 0) {
      clog_death(c->username, npc_name(n), c->floor_id);
      save_bones(c);
      player_respawn_town(c);
      send_text_to_client(
          c->sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
    }
  } else {
    clog_attack(npc_name(n), c->username, roll_v, n_bonus, p_ac, false,
                false, 0);
    send_text_to_client(c->sock, "[COMBAT] %s missed you!",
                        npc_name(n));
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
void handle_boss_death(Client *c, NPC *boss) {
  (void)c;
  if (!boss || boss->archetype != ARCH_BOSS)
    return;
  if (boss->floor_id > 0 && boss->floor_id % 10 == 0) {
    int boss_index = (boss->floor_id / 10) - 1;
    if (boss_index >= 0 && boss_index < 10) {
      uint64_t gold_reward = boss->floor_id * 1000;
      const char *boss_name = (boss->custom_name[0] != '\0')
                               ? boss->custom_name
                               : (boss->template ? boss->template->name : "Boss");
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].floor_id == boss->floor_id) {
          g_clients[i].gold += gold_reward;
          g_clients[i].bosses_defeated |= (1u << boss_index);
          send_text_to_client(g_clients[i].sock,
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
