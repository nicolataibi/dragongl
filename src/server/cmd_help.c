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
 * cmd_help.c - The reference of the text commands, in one table.
 *
 * COMMAND_DOCS says what every command is for. The in-game `help`, the
 * usage lines of docs/COMMANDS.md (`dragongl_server --print-commands`)
 * and the test that no command is undocumented all read it. The table of
 * server_commands.c decides which handler runs; this one only describes.
 */

#include "server_cmd.h"
#include "server_commands.h"
#include "server_internal.h"
#include <stdio.h>
#include <string.h>

static const struct {
    const char *topic; /*the word after "help"*/
    const char *title;
} GROUPS[CMD_GROUP_COUNT] = {
    [CMD_GROUP_WORLD] = {"world", "Exploring"},
    [CMD_GROUP_CHARACTER] = {"character", "Character"},
    [CMD_GROUP_ITEMS] = {"items", "Items and equipment"},
    [CMD_GROUP_MAGIC] = {"magic", "Magic and class rituals"},
    [CMD_GROUP_TOWN] = {"town", "Merchants and services"},
    [CMD_GROUP_TRADE] = {"trade", "Trade between players"},
    [CMD_GROUP_SOCIAL] = {"social", "Party and chat"},
    [CMD_GROUP_DM] = {"dm", "Dungeon Master"},
};

static const CmdDoc COMMAND_DOCS[] = {
    {"look", "l", CMD_GROUP_WORLD, "look",
     "What you see around you, by direction, and what lies under your feet."},
    {"examine", "ex", CMD_GROUP_WORLD, "examine",
     "The terrain one step away in every direction: what a wall is made of and what mining it "
     "could yield."},
    {"search", "s", CMD_GROUP_WORLD, "search", "Look for hidden traps and secret passages nearby."},
    {"open", "o", CMD_GROUP_WORLD, "open <n|s|e|w>", "Open the door in that direction."},
    {"close", "c", CMD_GROUP_WORLD, "close <n|s|e|w>", "Close a door you opened."},
    {"tunnel", "T|t", CMD_GROUP_WORLD, "tunnel <n|s|e|w|d>",
     "Mine the wall in that direction, or dig down (d), with a pickaxe or a shovel."},
    {"disarm", "D", CMD_GROUP_WORLD, "disarm [salvage] [magic]",
     "Disarm an adjacent trap: with your hands, or with 'magic' by dispelling it; 'salvage' also "
     "recovers its parts, at a harder roll. A bad roll springs the trap."},
    {"get", "g", CMD_GROUP_WORLD, "get [n|ne|e|se|s|sw|w|nw] [n]",
     "Pick up what lies on your tile, or on the adjacent one in that direction; with several "
     "things there, the one numbered n."},
    {"pickup", NULL, CMD_GROUP_WORLD, "pickup",
     "Take back what your tombstone holds, standing on it."},
    {"retrieve", NULL, CMD_GROUP_WORLD, "retrieve",
     "Recover your soul and equipment from your ghost, next to it."},
    {"fountain", NULL, CMD_GROUP_WORLD, "fountain",
     "Drink from the sacred fountain of the town (floor 0) to heal your wounds."},
    {"statue", "crystal", CMD_GROUP_WORLD, "statue",
     "Meditate at a crystal statue of the town: no fatigue, no hunger, spell slots restored."},
    {"rest", "R", CMD_GROUP_WORLD, "rest",
     "In town (floor 0): recover all hit points and spell slots."},
    {"pray", "p", CMD_GROUP_WORLD, "pray",
     "Sacrifice 100 XP to the gods to recover a quarter of your hit points."},

    {"status", "stats", CMD_GROUP_CHARACTER, "status",
     "Your character sheet: abilities and modifiers, armour class, conditions."},
    {"train", NULL, CMD_GROUP_CHARACTER, "train <str|dex|con|int|wis|cha|random>",
     "Spend a point earned at a level-up on an ability (the full name works too)."},
    {"save", NULL, CMD_GROUP_CHARACTER, "save", "Save your character now."},
    {"top", "leaderboard", CMD_GROUP_CHARACTER, "top", "The ranking of the players."},
    {"event", NULL, CMD_GROUP_CHARACTER, "event", "The global event in progress, if any."},
    {"help", "?", CMD_GROUP_CHARACTER, "help [topic|command|all]",
     "The topics; the commands of a topic; what one command does; every command."},

    {"inventory", "i", CMD_GROUP_ITEMS, "inventory", "Your backpack, belt and equipment."},
    {"wear", "equip|wield|w", CMD_GROUP_ITEMS, "wear <n|item>",
     "Put on or wield an item of the backpack."},
    {"remove", "unequip|takeoff", CMD_GROUP_ITEMS, "remove <slot|item>",
     "Take off an equipped or belted item."},
    {"belt", NULL, CMD_GROUP_ITEMS, "belt <n>",
     "Move an item from the backpack to the belt, where it is at hand."},
    {"unbelt", NULL, CMD_GROUP_ITEMS, "unbelt [n|item]",
     "Move an item from the belt back to the backpack."},
    {"use", "quaff|q|read|r|eat|e|zap|z", CMD_GROUP_ITEMS, "use <n|item>",
     "Use an item: drink a potion, read a scroll, eat, zap a wand."},
    {"drop", "d", CMD_GROUP_ITEMS, "drop <n>", "Drop an item of the backpack on the ground."},
    {"throw", "v", CMD_GROUP_ITEMS, "throw <n|item> [n|s|e|w]",
     "Throw an item: 5 tiles, 8 for ammunition. It lands where its flight ends."},
    {"fill", NULL, CMD_GROUP_ITEMS, "fill <lantern>", "Refill a lantern with a flask of oil."},
    {"lore", NULL, CMD_GROUP_ITEMS, "lore <n|item>",
     "What is known about an identified item of the backpack; for a book, its spells."},

    {"spells", NULL, CMD_GROUP_MAGIC, "spells", "The spells you know."},
    {"cast", "c", CMD_GROUP_MAGIC, "cast <spell> [target]",
     "Cast a spell you know; the book that holds it must be with you."},
    {"study", "awaken|invoke|intone|celebrate|pronounce|practice|drill|rage|sneak|meditate",
     CMD_GROUP_MAGIC, "study <n|book>",
     "Learn the spells of a book, inside the temple of your class in town, with the word of "
     "your class: study (Wizard), pray <n> (Cleric), awaken (Druid), invoke (Warlock), intone "
     "(Bard), celebrate (Paladin), pronounce (Sorcerer), practice (Ranger), drill (Fighter), "
     "rage (Barbarian), sneak (Rogue), meditate (Monk)."},

    {"talk", NULL, CMD_GROUP_TOWN, "talk", "Greet the merchant next to you."},
    {"list", NULL, CMD_GROUP_TOWN, "list", "The goods of the merchant next to you."},
    {"browse", NULL, CMD_GROUP_TOWN, "browse <n>", "The description of an item for sale."},
    {"buy", NULL, CMD_GROUP_TOWN, "buy <n|item>", "Buy an item from the merchant."},
    {"sell", NULL, CMD_GROUP_TOWN, "sell <n|item>", "Sell an item of the backpack."},
    {"haggle", NULL, CMD_GROUP_TOWN, "haggle buy|sell <n|item>",
     "Bargain on one deal (Charisma): the next buy or sell of that item has the new price, "
     "better or worse."},
    {"identify", NULL, CMD_GROUP_TOWN, "identify [n]",
     "Have the first unidentified item identified, or the one numbered n (100 gold)."},
    {"repair", NULL, CMD_GROUP_TOWN, "repair [slot|item]",
     "Have a worn piece of equipment repaired by the blacksmith of the town."},
    {"cure", NULL, CMD_GROUP_TOWN, "cure", "Have diseases and poisons cured (50 gold)."},
    {"uncurse", NULL, CMD_GROUP_TOWN, "uncurse", "Have a curse removed (200 gold)."},

    {"trade", NULL, CMD_GROUP_TRADE, "trade <player>", "Propose a trade to a player next to you."},
    {"offer", NULL, CMD_GROUP_TRADE, "offer <gold> [n]",
     "Put gold, and item n of the backpack, on the table."},
    {"accept", NULL, CMD_GROUP_TRADE, "accept", "Accept the trade as it stands."},
    {"cancel", NULL, CMD_GROUP_TRADE, "cancel", "Call the trade off."},

    {"say", "shout", CMD_GROUP_SOCIAL, "say <text>",
     "Speak to the players near you on this floor."},
    {"msg", "tell|whisper", CMD_GROUP_SOCIAL, "msg <player> <text>",
     "A private message to a player online."},
    {"who", NULL, CMD_GROUP_SOCIAL, "who", "The players online and their floors."},
    {"block", NULL, CMD_GROUP_SOCIAL, "block <player>", "Stop receiving the messages of a player."},
    {"unblock", NULL, CMD_GROUP_SOCIAL, "unblock <player>", "Receive them again."},
    {"blocks", "blocklist", CMD_GROUP_SOCIAL, "blocks", "The players you have blocked."},
    {"invite", NULL, CMD_GROUP_SOCIAL, "invite <player>", "Invite a player to your party."},
    {"join", NULL, CMD_GROUP_SOCIAL, "join", "Accept the last invitation."},
    {"leave", NULL, CMD_GROUP_SOCIAL, "leave", "Leave your party."},
    {"party", NULL, CMD_GROUP_SOCIAL, "party", "The members of your party."},

    {"dm_goto", NULL, CMD_GROUP_DM, "dm_goto <floor> <x> <y>", "Teleport."},
    {"dm_spawn", NULL, CMD_GROUP_DM, "dm_spawn <bestiary_id> <x> <y>",
     "Create a monster on your floor."},
    {"dm_spawn_boss", NULL, CMD_GROUP_DM, "dm_spawn_boss <floor>",
     "Create the boss of a floor again."},
    {"dm_boss", NULL, CMD_GROUP_DM, "dm_boss <floor>",
     "The bosses of a boss floor (10, 20, ...): where they are, their statistics and loot."},
    {"dm_merchant", NULL, CMD_GROUP_DM, "dm_merchant <floor>",
     "The wandering merchant of a floor (5, 15, 25, ...)."},
    {"dm_find_monster", NULL, CMD_GROUP_DM, "dm_find_monster <name>",
     "Bestiary entries whose name contains the text."},
    {"dm_find_item", NULL, CMD_GROUP_DM, "dm_find_item <name>",
     "Items whose name contains the text."},
    {"dm_shop", NULL, CMD_GROUP_DM, "dm_shop <item_id> [quantity]",
     "Add an item to the goods of the merchant within 5 steps."},
    {"dm_place", NULL, CMD_GROUP_DM, "dm_place <item_id> <x> <y>",
     "Place a treasure chest that holds the item."},
    {"dm_gold", NULL, CMD_GROUP_DM, "dm_gold <amount>", "Add gold to your purse, or remove it."},
    {"dm_level", NULL, CMD_GROUP_DM, "dm_level <level>", "Raise your level (20 at most)."},
    {"dm_pdf", NULL, CMD_GROUP_DM, "dm_pdf <floor>", "Write the colour map of a floor as a PDF."},
    {"dm_mapfloor", NULL, CMD_GROUP_DM, "dm_mapfloor", "The same, for the floor you are on."},
    {"dm_telemetry", NULL, CMD_GROUP_DM, "dm_telemetry",
     "Round and event-loop times, queues, clients and NPCs."},
};

#define DOC_COUNT (sizeof(COMMAND_DOCS) / sizeof(COMMAND_DOCS[0]))

const CmdDoc *cmd_docs(size_t *n) {
    *n = DOC_COUNT;
    return COMMAND_DOCS;
}

const char *cmd_group_topic(CmdGroup group) {
    return GROUPS[group].topic;
}

/*True if word is one of the names of "a|b|c".*/
static bool in_list(const char *list, const char *word) {
    size_t len = strlen(word);
    for (const char *p = list; p && len > 0;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == len && strncmp(p, word, n) == 0) {
            return true;
        }
        p = bar ? bar + 1 : NULL;
    }
    return false;
}

const CmdDoc *cmd_doc_find(const char *word) {
    for (size_t i = 0; i < DOC_COUNT; i++) {
        if (strcmp(COMMAND_DOCS[i].name, word) == 0) {
            return &COMMAND_DOCS[i];
        }
    }
    for (size_t i = 0; i < DOC_COUNT; i++) {
        if (in_list(COMMAND_DOCS[i].aliases, word)) {
            return &COMMAND_DOCS[i];
        }
    }
    return NULL;
}

/*"a|b|c" as "a, b, c".*/
static void aliases_text(const char *aliases, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (const char *p = aliases; p && *p && n + 3 < cap; p++) {
        if (*p == '|') {
            out[n++] = ',';
            out[n++] = ' ';
        } else {
            out[n++] = *p;
        }
        out[n] = '\0';
    }
}

static bool visible(const Client *c, CmdGroup group) {
    return group != CMD_GROUP_DM || c->is_dm;
}

/*"help": the topics, each with the names of its commands.*/
static void help_topics(const Client *c) {
    send_text_to_client(c->sock, "[HELP] 'help <topic>' lists the commands of a topic, 'help "
                                 "<command>' explains one, 'help all' lists everything.");
    for (int g = 0; g < CMD_GROUP_COUNT; g++) {
        if (!visible(c, (CmdGroup)g)) {
            continue;
        }
        char line[400];
        int n = snprintf(line, sizeof(line), "[HELP] %s:", GROUPS[g].topic);
        for (size_t i = 0; i < DOC_COUNT && n > 0 && (size_t)n < sizeof(line); i++) {
            if (COMMAND_DOCS[i].group == (CmdGroup)g) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", COMMAND_DOCS[i].name);
            }
        }
        send_text_to_client(c->sock, "%s", line);
    }
    send_text_to_client(c->sock, "[HELP] You move with the keys of the client, not with a "
                                 "command (F1 in its window lists them).");
}

static void help_command(const Client *c, const CmdDoc *d) {
    send_text_to_client(c->sock, "[HELP] %s - %s", d->synopsis, d->help);
    if (d->aliases) {
        char names[160];
        aliases_text(d->aliases, names, sizeof(names));
        send_text_to_client(c->sock, "[HELP] Also: %s", names);
    }
}

static void help_group(const Client *c, CmdGroup g) {
    send_text_to_client(c->sock, "[HELP] --- %s ---", GROUPS[g].title);
    for (size_t i = 0; i < DOC_COUNT; i++) {
        if (COMMAND_DOCS[i].group == g) {
            help_command(c, &COMMAND_DOCS[i]);
        }
    }
}

/*help | help <topic> | help <command> | help all ("?" works like "help").*/
bool cmd_help(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *arg = strchr(cmd, ' ');
    while (arg && *arg == ' ') {
        arg++;
    }
    if (!arg || !*arg) {
        help_topics(c);
        return true;
    }
    if (strcmp(arg, "all") == 0) {
        for (int g = 0; g < CMD_GROUP_COUNT; g++) {
            if (visible(c, (CmdGroup)g)) {
                help_group(c, (CmdGroup)g);
            }
        }
        return true;
    }
    for (int g = 0; g < CMD_GROUP_COUNT; g++) {
        if (strcmp(arg, GROUPS[g].topic) == 0 && visible(c, (CmdGroup)g)) {
            help_group(c, (CmdGroup)g);
            return true;
        }
    }
    const CmdDoc *d = cmd_doc_find(arg);
    if (d && visible(c, d->group)) {
        help_command(c, d);
        return true;
    }
    send_text_to_client(
        c->sock, "[HELP] '%s' is neither a command nor a topic. Type 'help' for the topics.", arg);
    return true;
}

/*A cell of a Markdown table: '|' would end it; in running text '<n>'
 * would be read as an HTML tag.*/
static void md_cell(FILE *out, const char *text, bool code) {
    for (const char *p = text; *p; p++) {
        if (*p == '|') {
            fputs("\\|", out);
        } else if (*p == '<' && !code) {
            fputs("&lt;", out);
        } else {
            fputc(*p, out);
        }
    }
}

void cmd_help_markdown(FILE *out) {
    fputs("# Commands\n\n"
          "<!-- Generated by `dragongl_server --print-commands` from the table of\n"
          "     src/server/cmd_help.c. Do not edit: change the table, then run\n"
          "     `tools/quality.sh commands-fix`. -->\n\n"
          "Commands are typed in the window of the client (Enter opens the command\n"
          "line) or in its terminal. A leading `/` is accepted: `/look` is `look`.\n"
          "A command typed without what it needs answers with its usage. Movement\n"
          "and the camera have keys, not commands: F1 in the client lists them.\n\n"
          "In the game, `help` lists the topics below, `help <topic>` the commands\n"
          "of one, `help <command>` explains a command.\n\n"
          "`<n>` is the number an item has in the list the command works on (the\n"
          "backpack, the goods of a merchant, the things on the ground); where\n"
          "`item` is allowed, part of its name works too.\n",
          out);
    for (int g = 0; g < CMD_GROUP_COUNT; g++) {
        fprintf(out, "\n## %s\n\n", GROUPS[g].title);
        if (g == CMD_GROUP_DM) {
            fputs("Only for the `dm` account (see the server manual for its password).\n\n", out);
        }
        fputs("| Command | Also | What it does |\n| :--- | :--- | :--- |\n", out);
        for (size_t i = 0; i < DOC_COUNT; i++) {
            const CmdDoc *d = &COMMAND_DOCS[i];
            if (d->group != (CmdGroup)g) {
                continue;
            }
            fputs("| `", out);
            md_cell(out, d->synopsis, true);
            fputs("` | ", out);
            /*Each alias in its own code span.*/
            for (const char *p = d->aliases; p && *p;) {
                size_t len = strcspn(p, "|");
                fprintf(out, "`%.*s`", (int)len, p);
                p += len;
                if (*p == '|') {
                    fputs(", ", out);
                    p++;
                }
            }
            fputs(" | ", out);
            md_cell(out, d->help, false);
            fputs(" |\n", out);
        }
    }
}
