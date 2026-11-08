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
 * cmd_character.c - Character commands: help, train, stats, rest, save.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "server_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

bool cmd_help(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  send_text_to_client(c->sock, "[HELP] --- BASIC COMMANDS ---");
  send_text_to_client(c->sock, "  l, look         : Observe the surrounding environment");
  send_text_to_client(c->sock, "  ex, examine     : Examine terrain and minerals 1 step away");
  send_text_to_client(c->sock, "stats : Shows the character's statistics");
  send_text_to_client(c->sock, "s, search : Search for secret passages or traps");
  send_text_to_client(c->sock, "train <stat> : Use level up stat points (e.g. train str / random)");
  send_text_to_client(c->sock, "t/T, tunnel <dir> : Dig a wall (t n = tunnel north, d = down)");
  send_text_to_client(c->sock, "o, open <n|s|e|w> : Open a closed door");
  send_text_to_client(c->sock, "close <n|s|e|w> : Close a door you opened");
  send_text_to_client(c->sock, "D, disarm : Disarm at trap");
  send_text_to_client(c->sock, "R, rest : Rest to recover HP/Mana");
  send_text_to_client(c->sock, "fountain : Drink from the sacred fountain to heal wounds (Floor 0)");
  send_text_to_client(c->sock, "statue/crystal : Meditate at the crystal statues (Floor 0)");
  send_text_to_client(c->sock, "save : Save the game");

  send_text_to_client(c->sock, "[HELP] --- ITEMS AND INVENTORY ---");
  send_text_to_client(c->sock, "i, inventory : Show inventory");
  send_text_to_client(c->sock, "  w, wield/wear   : Wield/wear an item (e.g. 'w 2')");
  send_text_to_client(c->sock, "remove/unequip/takeoff <slot|item> : Remove equipped or belted item");
  send_text_to_client(c->sock, "belt <n> : Move an item from the backpack to the speed belt");
  send_text_to_client(c->sock, "unbelt <n> : Move an item from the belt to the backpack");
  send_text_to_client(c->sock, "d, drop <n> : Drops object <n> to the ground");
  send_text_to_client(c->sock, "v, throw <n> : Throw an object (e.g. 'v 1')");
  send_text_to_client(c->sock, "fill <n> : Refills a lantern with oil (e.g. 'fill lantern')");
  send_text_to_client(c->sock, "use <n> : Use an item (potions, food, wands)");
  send_text_to_client(c->sock, "q, quaff <n> : Drink a potion (aka for 'use')");
  send_text_to_client(c->sock, "  r, read <n>     : Read book/scroll (e.g. 'read 1')");
  send_text_to_client(c->sock, "e, eat <n> : Eat food (aka for 'use')");
  send_text_to_client(c->sock, "z, zap <n> : Use wand (aka for 'use')");

  send_text_to_client(c->sock, "[HELP] --- MAGIC AND CURES ---");
  send_text_to_client(c->sock, "spells : Show your library (spells known)");
  send_text_to_client(c->sock, "cast <spell> : Cast a spell from the library (e.g. 'cast Fire Bolt')");
  send_text_to_client(c->sock, "study <n> : Learn magic (Wizard)");
  send_text_to_client(c->sock, "pray <n> : Learn spells (Cleric)");
  send_text_to_client(c->sock, "awaken <n> : Learn magic (Druid)");
  send_text_to_client(c->sock, "invoke <n> : Learn magic (Warlock)");
  send_text_to_client(c->sock, "intone <n> : Learn magic (Bard)");
  send_text_to_client(c->sock, "celebrate <n> : Learn magic (Paladin)");
  send_text_to_client(c->sock, "pronounce <n> : Learn magic (Sorcerer)");
  send_text_to_client(c->sock, "practice <n> : Learn magic (Ranger)");
  send_text_to_client(c->sock, "  drill <n>       : Execute maneuvers (Fighter)");
  send_text_to_client(c->sock, "rage <n> : Channel fury (Barbarian)");
  send_text_to_client(c->sock, "  sneak <n>       : Hone shadows (Rogue)");
  send_text_to_client(c->sock, "  meditate <n>    : Learn disciplines (Monk)");
  send_text_to_client(c->sock, "lore <n> : Examine a book or inventory item");
  send_text_to_client(c->sock, "cures: Cure diseases/poisons at the temple (cost: gold)");
  send_text_to_client(c->sock, "uncurse : Remove curses from the temple (cost: gold)");
  send_text_to_client(c->sock, "retrieve : Retrieve your items and soul from your ghost");

  send_text_to_client(c->sock, "[HELP] --- INTERACTIONS WITH NPCs AND STORES ---");
  send_text_to_client(c->sock, "  talk            : Talk with nearby merchant or NPC");
  send_text_to_client(c->sock, "list : Show goods for sale");
  send_text_to_client(c->sock, "browse <n> : Examine an item for sale");
  send_text_to_client(c->sock, "buy <n> : Buys the item for sale (e.g. 'buy sword')");
  send_text_to_client(c->sock, "sell <n> : Sell an item (e.g. 'sell dagger')");
  send_text_to_client(c->sock, "haggle buy/sell : Haggle to get a better price");
  send_text_to_client(c->sock, "identify <n> : Identifies an unknown object");
  send_text_to_client(c->sock, "repair <n> : Repairs a damaged object");

  send_text_to_client(c->sock, "[HELP] --- TRADE BETWEEN PLAYERS ---");
  send_text_to_client(c->sock, "trade <name> : Request to trade with a nearby player");
  send_text_to_client(c->sock, "offer <gold> [id]: Offer gold and/or an item (from your backpack)");
  send_text_to_client(c->sock, "accept : Accept the exchange offer");
  send_text_to_client(c->sock, "cancel : Cancel the exchange");

  send_text_to_client(c->sock, "[HELP] --- MESSAGING AND SOCIAL ---");
  send_text_to_client(
      c->sock,
      "msg <name> <text> : Private message to an online player (aka 'tell'/'whisper')");
  send_text_to_client(
      c->sock,
      "say <text> : Say something to nearby players on this floor (aka 'shout')");
  send_text_to_client(c->sock, "who : List the players currently online");
  send_text_to_client(
      c->sock,
      "block <name> : Block a player (their messages will no longer reach you)");
  send_text_to_client(
      c->sock, "unblock <name> : Remove a player from the block list");
  send_text_to_client(c->sock, "blocks : Show your block list");

  send_text_to_client(c->sock, "[HELP] --- EVENTS AND RANKING ---");
  send_text_to_client(c->sock, "event : Shows the status of the current global event");
  send_text_to_client(c->sock, "top : Shows the global ranking of players");

  if (c->is_dm) {
      send_text_to_client(c->sock, "[DM] --- MASTER COMMANDS ---");
      send_text_to_client(c->sock, "  dm_spawn <id> <x> <y> : Spawn entity/monster");
      send_text_to_client(c->sock, "dm_goto <fl> <x> <y> : Teleport (fl = floor)");
      send_text_to_client(c->sock, "dm_find_monster <id> : Search for the monster");
      send_text_to_client(c->sock, "  dm_find_item <id>     : Search for an item");
      send_text_to_client(c->sock, "dm_pdf <floor> : Generate color PDF of a floor");
      send_text_to_client(c->sock, "dm_mapfloor : Generate color PDF of the current floor");
      send_text_to_client(c->sock, "dm_shop : Spawns a custom merchant");
      send_text_to_client(c->sock, "  dm_place <id>         : Place world object (furniture)");
      send_text_to_client(c->sock, "dm_gold <amt> : Add/remove gold coins");
      send_text_to_client(c->sock, "dm_level <lvl> : Increase level up to lvl (max 20)");
      send_text_to_client(c->sock, "  dm_telemetry          : Show server metrics (RAM/CPU)");
      send_text_to_client(c->sock, "dm_boss <floor> : Locate bosses on the indicated floor (stats+loot)");
  }
  return true;
}

//train <stat> or train random — awards unspent stat points
bool cmd_train(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *stat = cmd + 5;
  while (*stat == ' ') stat++;

  if (c->unspent_stat_points <= 0) {
    send_text_to_client(c->sock, "[SYSTEM] You have no stat points to assign.");
    return true;
  }
  if (strlen(stat) == 0) {
    send_text_to_client(c->sock, "[SYSTEM] Specify a stat (str, dex, con, int, wis, cha) or 'random'. You have %d points.", c->unspent_stat_points);
    return true;
  }
    int chosen = -1; //0=str, 1=dex, 2=con, 3=int, 4=wis, 5=cha
    if (strncasecmp(stat, "str", 3) == 0) chosen = 0;
    else if (strncasecmp(stat, "dex", 3) == 0) chosen = 1;
    else if (strncasecmp(stat, "with", 3) == 0) chosen = 2;
    else if (strncasecmp(stat, "int", 3) == 0) chosen = 3;
    else if (strncasecmp(stat, "wis", 3) == 0) chosen = 4;
    else if (strncasecmp(stat, "cha", 3) == 0) chosen = 5;
    else if (strncasecmp(stat, "random", 6) == 0) chosen = rand() % 6;

    if (chosen == -1) {
      send_text_to_client(c->sock, "[SYSTEM] Unknown stat. Use str, dex, con, int, wis, cha or random.");
      return true;
    }

    c->unspent_stat_points--;
    char *stat_name = "";
    /*rules_get_modifier(): floor((score-10)/2), not a division that
     * truncates toward zero (CON 9->10 changes the modifier).*/
    int old_mod = rules_get_modifier(c->con);

    if (chosen == 0) { c->str++; stat_name = "Strength"; }
    else if (chosen == 1) { c->dex++; stat_name = "Dexterity"; }
    else if (chosen == 2) {
        c->con++;
        stat_name = "Constitution";
        int new_mod = rules_get_modifier(c->con);
        if (new_mod > old_mod) {
            c->max_hp += c->level;
            c->hp += c->level;
            send_text_to_client(c->sock, "[SYSTEM] Your Constitution makes you more resistant! Max HP +%d.", c->level);
        }
    }
    else if (chosen == 3) { c->intel++; stat_name = "Intelligence"; }
    else if (chosen == 4) { c->wis++; stat_name = "Wisdom"; }
    else if (chosen == 5) { c->cha++; stat_name = "Charisma"; }

    send_text_to_client(c->sock, "[SYSTEM] You have increased %s! (Points remaining: %d)", stat_name, c->unspent_stat_points);
    save_player_data(c);
    send_detailed_state(c);
    return true;
}

bool cmd_stats(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  int ts, td, tc, ti, tw, th;
  get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
  //Calculate AC
  int ac_base = (c->slot_body.template_idx != -1)
                    ? item_database[c->slot_body.template_idx].ac_base
                    : 0;
  int ac_bonus = 0;
  if (c->slot_head.template_idx != -1)
    ac_bonus += item_database[c->slot_head.template_idx].ac_bonus + c->slot_head.ac_bonus;
  if (c->slot_body.template_idx != -1)
    ac_bonus += c->slot_body.ac_bonus;
  if (c->slot_hand_r.template_idx != -1 &&
      item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD)
    ac_bonus += item_database[c->slot_hand_r.template_idx].ac_bonus + c->slot_hand_r.ac_bonus;
  if (c->slot_hand_l.template_idx != -1 &&
      item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD)
    ac_bonus += item_database[c->slot_hand_l.template_idx].ac_bonus + c->slot_hand_l.ac_bonus;
  int ac = (ac_base > 0 ? ac_base : 10 + rules_get_modifier(td)) + ac_bonus;
  //Weapon equipped
  const char *weapon_name = "Bare hands";
  char weapon_buf[128];
  if (c->slot_hand_r.template_idx != -1) {
    get_full_item_name(&c->slot_hand_r, weapon_buf, sizeof(weapon_buf));
    weapon_name = weapon_buf;
  }
  else if (c->slot_hand_l.template_idx != -1) {
    get_full_item_name(&c->slot_hand_l, weapon_buf, sizeof(weapon_buf));
    weapon_name = weapon_buf;
  }
  //Proficiency bonuses
  int prof = 2 + (c->level / 4);
  send_text_to_client(c->sock, "======== %s ========", c->username);
  send_text_to_client(c->sock, "Lvl:%-3d XP:%-6d Floor:%d", c->level,
                      c->xp, c->floor_id);
  send_text_to_client(c->sock, "HP: %d/%d AC:%d Prof:+%d Gold:%lu gp",
                      c->hp, c->max_hp, ac, prof, (unsigned long)c->gold);
  send_text_to_client(c->sock, "--- ATTRIBUTES ---");
  send_text_to_client(c->sock, "  STR %2d(%+d)  DEX %2d(%+d)  CON %2d(%+d)",
                      ts, rules_get_modifier(ts), td, rules_get_modifier(td),
                      tc, rules_get_modifier(tc));
  send_text_to_client(c->sock, "  INT %2d(%+d)  WIS %2d(%+d)  CHA %2d(%+d)",
                      ti, rules_get_modifier(ti), tw, rules_get_modifier(tw),
                      th, rules_get_modifier(th));
  send_text_to_client(c->sock, "--- EQUIPMENT ---");
  send_text_to_client(c->sock, "  Weapon: %s", weapon_name);
  if (c->slot_body.template_idx != -1) {
    char armor_buf[128];
    get_full_item_name(&c->slot_body, armor_buf, sizeof(armor_buf));
    send_text_to_client(c->sock, "Armor: %s (base AC %d)",
                        armor_buf,
                        ac_base);
  }
  //Spell Slots
  bool has_slots = false;
  for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
    if (c->spell_slots_max[s] > 0) {
      has_slots = true;
      break;
    }
  }
  if (has_slots) {
    send_text_to_client(c->sock, "--- SPELL SLOT ---");
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
      if (c->spell_slots_max[s] > 0)
        send_text_to_client(c->sock, "  Lvl%d: %d/%d", s, c->spell_slots[s],
                            c->spell_slots_max[s]);
    }
  }
  //Active effects
  if (c->effect_count > 0) {
    send_text_to_client(c->sock, "--- ACTIVE EFFECTS ---");
    for (int e = 0; e < c->effect_count; e++) {
      ActiveEffect *ef = &c->effects[e];
      send_text_to_client(c->sock, "  #%d val:%d (%s)", e + 1, ef->value,
                          ef->is_persistent ? "persistent" : "temporary");
    }
  }
  send_text_to_client(c->sock, "==============================");
  return true;
}

bool cmd_rest(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->floor_id == 0) {
    c->hp = c->max_hp;
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++)
      c->spell_slots[s] = c->spell_slots_max[s];
    send_detailed_state(c); //Sync HP and spell slots with the HUD client
    send_text_to_client(c->sock, "[SYSTEM] Rested.");
  }
  return true;
}

bool cmd_save(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  save_player_data(c);
  send_text_to_client(c->sock, "[SYSTEM] Saved.");
  return true;
}
