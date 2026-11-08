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
 * cmd_magic.c - Magic commands: spells, cast, pray.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "server_internal.h"
#include "aoe.h"
#include "spell_router.h"
#include "server_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*-------------------------------------------------------
 * spells — lists the spells in your personal grimoire
 * Show only spells in the known_spells bitfield, grouped together
 * per level with the number of slots remaining.
 * -------------------------------------------------------*/
bool cmd_spells(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  int listed = 0;
  /*Group by level: 0 (cantrip) .. MAX_SPELL_LEVEL*/
  for (int lv = 0; lv <= MAX_SPELL_LEVEL; lv++) {
    int lv_count = 0;
    /*First pass: Count how many spells this level has*/
    for (int i = 0; i < spell_database_size; i++) {
      if (spell_database[i].level != lv)
        continue;
      if (!spell_known_get(c, i))
        continue;
      lv_count++;
    }
    if (lv_count == 0)
      continue;
    /*Level header*/
    if (lv == 0)
      send_text_to_client(c->sock, "--- Cantrips (Slot: unlimited) ---");
    else
      send_text_to_client(c->sock, "--- Level %d (Slot: %d/%d) ---",
                          lv,
                          c->spell_slots[lv],
                          c->spell_slots_max[lv]);
    /*Second pass: List the spells*/
    for (int i = 0; i < spell_database_size; i++) {
      if (spell_database[i].level != lv)
        continue;
      if (!spell_known_get(c, i))
        continue;
      send_text_to_client(c->sock, "  cast %-30s  [Range: %d]",
                          spell_database[i].name,
                          spell_database[i].range);
      listed++;
    }
  }
  if (listed == 0)
    send_text_to_client(c->sock,
        "[GRIMORY] You don't know any magic yet."
        "Find a magical book and study it with 'study <n>' (in your classroom temple).");
  return true;
}

//cast <name> or cast <book_idx> <spell_idx>
bool cmd_cast(Client *c, const char *cmd, NPC *npcs) {
  //--- ACTION BLOCK FOR SILENCE ---
  if (rules_has_condition_t(c->effects, c->effect_count, COND_SILENCED)) {
    send_text_to_client(
        c->sock, "[SYSTEM] You are silenced and cannot cast spells!");
    return true;
  }
  const char *spell_name = strchr(cmd, ' ');
  if (spell_name) spell_name++; else return true;

  //Support for "cast <book_idx> <spell_idx>" (shortcut)
  int b_idx, l_idx;
  if (sscanf(spell_name, "%d %d", &b_idx, &l_idx) == 2) {
    b_idx--; //From 1-based to 0-based index in the backpack
    if (b_idx >= 0 && b_idx < c->backpack_count) {
      ItemTemplate *tmpl = &item_database[c->backpack[b_idx].template_idx];
      if (tmpl->category == ITEM_BOOK) {
         int local_idx = l_idx - (tmpl->book_seq * MAX_BOOK_SPELLS);
         if (local_idx >= 0 && local_idx < tmpl->book_spell_count) {
             spell_name = tmpl->book_spell_names[local_idx];
         }
      }
    }
  }

  int sp_idx = -1;
  for (int i = 0; i < spell_database_size; i++) {
    if (strcasecmp(spell_database[i].name, spell_name) == 0) {
      sp_idx = i;
      break;
    }
  }
  if (sp_idx == -1) {
    send_text_to_client(c->sock, "[ERROR] Spell '%s' not found.",
                        spell_name);
    return true;
  }
  SpellTemplate *sp = &spell_database[sp_idx];

  /*-------------------------------------------------------
   * GATEKEEPER: Check that the spell is in the library
   * of the character (known_spells bitfield).
   * spell_known_get() guards the index.
   * -------------------------------------------------------*/
  {
    if (!spell_known_get(c, sp_idx)) {
      send_text_to_client(c->sock,
          "[MAGIC] You don't know '%s'. Find the corresponding book"
          "and study it at the temple with 'study <n>'.",
          sp->name);
      return true;
    }
  }

  /*-------------------------------------------------------
   * BOOK REQUIREMENT: the book that holds this spell must be
   * in the backpack, on the belt or in a hand to cast it, the
   * class transit cantrips included.
   * -------------------------------------------------------*/
  bool has_book = false;

  //Check your backpack
  for (int i = 0; i < c->backpack_count; i++) {
    if (c->backpack[i].template_idx == -1) continue;
    ItemTemplate *tmpl = &item_database[c->backpack[i].template_idx];
    if (tmpl->category == ITEM_BOOK) {
      for (int k = 0; k < tmpl->book_spell_count; k++) {
        if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
          has_book = true; break;
        }
      }
    }
    if (has_book) break;
  }

  //Check your belt
  if (!has_book) {
    for (int i = 0; i < 4; i++) {
      if (c->belt[i].template_idx == -1) continue;
      ItemTemplate *tmpl = &item_database[c->belt[i].template_idx];
      if (tmpl->category == ITEM_BOOK) {
        for (int k = 0; k < tmpl->book_spell_count; k++) {
          if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
            has_book = true; break;
          }
        }
      }
      if (has_book) break;
    }
  }

  //Check in the hands (equipped)
  if (!has_book) {
      ItemInstance* hands[2] = { &c->slot_hand_l, &c->slot_hand_r };
      for (int i = 0; i < 2; i++) {
          if (hands[i]->template_idx == -1) continue;
          ItemTemplate *tmpl = &item_database[hands[i]->template_idx];
          if (tmpl->category == ITEM_BOOK) {
            for (int k = 0; k < tmpl->book_spell_count; k++) {
              if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
                has_book = true; break;
              }
            }
          }
          if (has_book) break;
      }
  }

  if (!has_book) {
    send_text_to_client(c->sock,
        "[MAGIC] You have memorized '%s', but the book containing it is not in your inventory! You have to carry it with you to throw it.",
        sp->name);
    return true;
  }


  /*Check spell slot*/
  int slv = sp->level;
  if (slv > 0 && c->spell_slots[slv] <= 0) {
    send_text_to_client(c->sock,
                        "[MAGIC] No level %d slots left.", slv);
    return true;
  }
  /*Consume the slot*/
  if (slv > 0)
    c->spell_slots[slv]--;

  //Find target: Closest active NPC in range
  NPC *target = NULL;
  int min_d = sp->range + 1;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT)
      continue;
    int d = abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y);
    if (d < min_d) {
      min_d = d;
      target = &npcs[i];
    }
  }


  int tx = c->x, ty = c->y;
  if (target && sp->target_type != SPELL_TARGET_SELF) { tx = target->x; ty = target->y; }

  broadcast_spell_vfx(c->x, c->y, tx, ty, sp->vfx_type, sp->vfx_r, sp->vfx_g, sp->vfx_b, c->floor_id);

  // -------------------------------------------------------
  //MAGIC ROUTER: spell dispatch with custom logic
  //O(log N) binary search in the hook table.
  //If the handler returns true, the handling is complete.
  // -------------------------------------------------------
  {
    SpellContext router_ctx;
    router_ctx.caster = c;
    router_ctx.sp     = sp;
    router_ctx.npcs   = npcs;
    router_ctx.target = target;
    if (spell_router_dispatch(&router_ctx)) {
      check_level_up(c);
      return true;
    }
  }

  if (sp->target_type == SPELL_TARGET_SELF) {
    //Effect on oneself
    if (sp->effect_type == SPELL_EFFECT_HEAL) {
      int dice_hp = rules_roll_dice(sp->dice_count, sp->dice_sides);
      c->hp = (c->hp + dice_hp > c->max_hp) ? c->max_hp : c->hp + dice_hp;
      send_text_to_client(c->sock, "[MAGIC] %s: Recover %d HP (HP: %d/%d).",
                          sp->name, dice_hp, c->hp, c->max_hp);
      send_combat_event(c->sock, COMBAT_EV_HEAL, c->entity_id, dice_hp);
    } else if (sp->has_status_effect &&
               c->effect_count < MAX_EFFECTS_PER_ENTITY) {
      c->effects[c->effect_count++] = sp->status_effect;
      send_text_to_client(c->sock, "[MAGIC] %s active for %d rounds.",
                          sp->name, sp->status_effect.duration_rounds);
    } else {
      send_text_to_client(
          c->sock,
          "> You whisper arcane words and the energy of %s envelops you...",
          sp->name);
    }
    return true;
  }

  // -------------------------------------------------------
  //AoE: Circle, Cone, Line, Cloud -> aoe.c engine
  // -------------------------------------------------------
  if (sp->target_type == SPELL_TARGET_AOE_CIRCLE ||
      sp->target_type == SPELL_TARGET_AOE_CONE ||
      sp->target_type == SPELL_TARGET_AOE_LINE ||
      sp->target_type == SPELL_TARGET_AOE_CLOUD) {
    int hits = aoe_resolve_spell(sp, c, npcs, MAX_NPCS, c->x, c->y);
    // Proportional XP: level-up is already handled inside
    //aoe_apply_damage_to_npc Check level-up after each explosion
    check_level_up(c);
    (void)hits;
    return true;
  }

  // -------------------------------------------------------
  //SINGLE TARGET (SPELL_TARGET_SINGLE)
  // -------------------------------------------------------
  if (!target) {
    send_text_to_client(c->sock,
                        "[MAGIC] No targets in range (%d) of %s.",
                        sp->range, sp->name);
    return true;
  }

  if (sp->effect_type == SPELL_EFFECT_DAMAGE) {
    int spell_save_dc = 8 + 2 + rules_get_modifier(c->intel);
    bool saved = rules_roll_save(0, spell_save_dc, false, false, NULL);
    int raw_dmg = rules_roll_dice(sp->dice_count > 0 ? sp->dice_count : 1,
                                  sp->dice_sides > 0 ? sp->dice_sides : 6);
    int dmg = rules_calculate_damage(raw_dmg, DMG_MOD_NORMAL);
    if (saved)
      dmg /= 2;
    target->hp -= dmg;
    send_text_to_client(
        c->sock, "[ACTION] Call upon the arcane forces and unleash %s against %s!",
        sp->name, target->template ? target->template->name : "???");
    send_text_to_client(
        c->sock,
        " > Mystic energy impacts for %d damage %s (HP: %d/%d).",
        dmg, saved ? "[Halved on Saving Throw!]" : "", target->hp,
        target->max_hp);
    clog_spell(c->username, sp->name,
               target->template ? target->template->name : "???", dmg, saved);
    if (target->hp <= 0) {
      npc_set_active(target, false);
      target->respawn_timer = RESPAWN_TICKS;
      int gained_xp = target->template ? target->template->xp : 10;
      c->xp += gained_xp;
      check_level_up(c);
      send_text_to_client(c->sock,
                          "[VICTORY] The power of the spell disintegrates"
                          " %s, reducing it to ashes! (+%d XP)",
                          target->template ? target->template->name : "???",
                          gained_xp);
      clog_death(target->template ? target->template->name : "???",
                 c->username, target->floor_id);
      if (target->archetype == ARCH_BOSS) {
        handle_boss_death(c, target);
      }
    }
  } else if (sp->effect_type == SPELL_EFFECT_HEAL) {
    int dice_hp = rules_roll_dice(sp->dice_count, sp->dice_sides);
    c->hp = (c->hp + dice_hp > c->max_hp) ? c->max_hp : c->hp + dice_hp;
    send_text_to_client(c->sock, "[MAGIC] %s: Recover %d HP.", sp->name,
                        dice_hp);
    send_combat_event(c->sock, COMBAT_EV_HEAL, c->entity_id, dice_hp);
  } else if (sp->has_status_effect) {
    if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
      target->effects[target->effect_count++] = sp->status_effect;
      send_text_to_client(c->sock, "[MAGIC] %s applied to %s for %d rounds.",
                          sp->name,
                          target->template ? target->template->name : "???",
                          sp->status_effect.duration_rounds);
    }
  }
  return true;
}

bool cmd_pray(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->xp >= 100) {
    c->xp -= 100;
    int heal = c->max_hp / 4;
    c->hp += heal;
    if (c->hp > c->max_hp) c->hp = c->max_hp;
    send_detailed_state(c); //Sync HP and XP with the HUD client
    send_text_to_client(c->sock, "[SYSTEM] You pray hard, sacrificing 100 XP. Restores %d HP (HP: %d/%d).", heal, c->hp, c->max_hp);
  } else {
    send_text_to_client(c->sock, "[SYSTEM] The Gods do not respond. You don't have enough experience (XP) to sacrifice.");
  }
  return true;
}
