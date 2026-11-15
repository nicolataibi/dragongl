/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The commands against their documentation (see harness_spec.h). Each test
 * names the line of docs/COMMANDS.md it holds the server to.
 */

#include "harness_spec.h"

#include "harness_util.h"

#include "classes.h"
#include "inventory.h"
#include "items.h"
#include "player.h"
#include "server_cmd.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "server_traps.h"
#include "spells.h"
#include "traps.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static VoxelType tile(int floor, int x, int y) {
    return g_game.world->floors[floor].map.data[0][y][x];
}

static void set_tile(int floor, int x, int y, VoxelType v) {
    g_game.world->floors[floor].map.data[0][y][x] = v;
}

static NPC *merchant_next(int slot, int floor, int x, int y, int stock_item) {
    NPC *m = &g_game.npcs[slot];
    memset(m, 0, sizeof(*m));
    m->archetype = ARCH_MERCHANT;
    m->entity_id = 9500 + slot;
    m->floor_id = floor;
    m->x = m->spawn_x = x;
    m->y = m->spawn_y = y;
    m->hp = m->max_hp = 100;
    m->respawn_timer = -1;
    MerchantData *shop = npc_shop_new(&g_game.pools, g_game.npcs, MAX_NPCS, m);
    snprintf(shop->shop_name, sizeof(shop->shop_name), "Spec Shop");
    if (stock_item >= 0) {
        add_item_to_shop(shop, stock_item, 5);
    }
    npc_set_active(&g_game, m, true);
    g_game.world->floors[floor].entity_grid[y][x] = m->entity_id;
    return m;
}

static void move_npc(NPC *n, int x, int y) {
    n->x = x;
    n->y = y;
}

static void afflict(Client *c, const char *name) {
    c->effects[c->effect_count++] =
        (ActiveEffect){name, EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
}

static bool has_effect(const Client *c, const char *name) {
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].name && strcmp(c->effects[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static int spell_named(const char *name) {
    for (int i = 0; i < spell_database_size; i++) {
        if (strcmp(spell_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* The caster knows the spell, carries its book, is of a class that casts
 * it and has every slot. */
static bool learn(Client *c, const char *name) {
    int si = spell_named(name);
    int book = -1;
    for (int i = 0; si >= 0 && i < item_database_size && book < 0; i++) {
        const ItemTemplate *t = &item_database[i];
        for (int k = 0; t->category == ITEM_BOOK && k < t->book_spell_count; k++) {
            if (strcmp(t->book_spell_names[k], name) == 0) {
                book = i;
            }
        }
    }
    if (book < 0) {
        return false;
    }
    for (int k = 0; k < CLASS_COUNT; k++) {
        if (spell_database[si].class_mask & (1u << k)) {
            c->class_id = (ClassType)k;
            break;
        }
    }
    c->level = 20;
    for (int l = 0; l <= MAX_SPELL_LEVEL; l++) {
        c->spell_slots[l] = c->spell_slots_max[l] = 9;
    }
    spell_known_set(c, si);
    give(c, book, 1);
    return true;
}

/* close <n|s|e|w> "c"; disarm "D"; tunnel "T", "t"; a direction is
 * n|s|e|w (or the full word), nothing else. */
int test_spec_aliases_and_directions(void) {
    hx_reset_world();
    Client *c = hero(0, "aliases", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    set_tile(FLOOR, CX, CY - 1, VOXEL_DOOR);

    run(c, "open ne");
    CHECK(tile(FLOOR, CX, CY - 1) == VOXEL_DOOR && said("Use: open"), "'ne' opened north");
    run(c, "open nonsense");
    CHECK(tile(FLOOR, CX, CY - 1) == VOXEL_DOOR && said("Use: open"), "'nonsense' opened north");
    run(c, "o north");
    CHECK(tile(FLOOR, CX, CY - 1) == VOXEL_FLOOR && said("You opened the door"), "o north");
    run(c, "c n");
    CHECK(tile(FLOOR, CX, CY - 1) == VOXEL_DOOR && said("You closed the door"),
          "'c n' did not close the door");
    run(c, "open n");
    run(c, "close north");
    CHECK(tile(FLOOR, CX, CY - 1) == VOXEL_DOOR, "close north");
    /* "c" with anything but a direction is still cast. */
    run(c, "c fire bolt");
    CHECK(said("You don't know 'Fire Bolt'"), "'c <spell>' did not reach cast");

    run(c, "D");
    CHECK(said("No detected trap next to you"), "'D' is not disarm");
    run(c, "D salvage");
    CHECK(said("No detected trap next to you"), "'D salvage' is not disarm");
    run(c, "t");
    CHECK(said("Use: tunnel"), "'t' is not tunnel");
    run(c, "T");
    CHECK(said("Use: tunnel"), "'T' is not tunnel");
    run(c, "t n");
    CHECK(said("need a pickaxe or a shovel"), "'t n' is not tunnel");
    return 0;
}

/* search: "Look for hidden traps and secret passages nearby." A straight
 * wall between open ground hides a passage here and there; a good search
 * turns it into a door, a bad one does not; Find Traps reveals it too. */
int test_spec_secret_passages(void) {
    hx_reset_world();
    Client *c = hero(0, "seeker", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    Floor *fl = &g_game.world->floors[FLOOR];
    /* A long straight wall one row north of the player. */
    for (int x = 3; x < MAP_WIDTH - 3; x++) {
        set_tile(FLOOR, x, CY - 1, VOXEL_WALL);
    }
    int sx = -1;
    for (int x = 4; x < MAP_WIDTH - 4 && sx < 0; x++) {
        if (secret_passage_at(fl, FLOOR, x, CY - 1)) {
            sx = x;
        }
    }
    CHECK(sx >= 0, "no secret passage along a wall of %d tiles", MAP_WIDTH - 6);
    CHECK(!secret_passage_at(fl, FLOOR, sx, CY), "a passage in open ground");
    /* The same wall in town hides nothing. */
    Floor *town = &g_game.world->floors[0];
    for (int x = 3; x < MAP_WIDTH - 3; x++) {
        town->map.data[0][CY - 1][x] = VOXEL_WALL;
    }
    for (int x = 4; x < MAP_WIDTH - 4; x++) {
        CHECK(!secret_passage_at(town, 0, x, CY - 1), "a secret passage in town at x %d", x);
    }

    place(c, FLOOR, sx, CY);
    c->wis = 10;
    rules_force_d20(2);
    run(c, "search");
    rules_force_d20(0);
    CHECK(tile(FLOOR, sx, CY - 1) == VOXEL_WALL && said("don't notice anything"),
          "a bad search found the passage");
    rules_force_d20(20);
    run(c, "search");
    rules_force_d20(0);
    CHECK(tile(FLOOR, sx, CY - 1) == VOXEL_DOOR && said("discovered a secret passage"),
          "a good search missed the passage");
    /* Found once, it is a door: nothing more to find there. */
    CHECK(!secret_passage_at(fl, FLOOR, sx, CY - 1), "the door is still a secret");

    /* Find Traps (here from a scroll) reveals a passage without a roll. */
    int sx2 = -1;
    for (int x = sx + 1; x < MAP_WIDTH - 4 && sx2 < 0; x++) {
        if (secret_passage_at(fl, FLOOR, x, CY - 1)) {
            sx2 = x;
        }
    }
    CHECK(sx2 >= 0, "no second passage");
    place(c, FLOOR, sx2, CY);
    give(c, item_named("Scroll of Find Traps"), 1);
    run(c, "read find traps");
    CHECK(tile(FLOOR, sx2, CY - 1) == VOXEL_DOOR && said("secret passage revealed"),
          "Find Traps did not reveal the passage");
    return 0;
}

/* disarm: "Disarm an adjacent trap": two steps away is not adjacent, a
 * diagonal neighbour is; the words are salvage and magic. */
int test_spec_disarm_next_to_you(void) {
    hx_reset_world();
    Client *c = hero(0, "sapper", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    Floor *fl = &g_game.world->floors[FLOOR];
    Trap *t = &fl->traps[0];
    memset(t, 0, sizeof(*t));
    t->type = TRAP_SPIKES;
    t->x = CX + 2;
    t->y = CY;
    t->active = true;
    t->detected = true;
    t->detection_dc = 10;
    fl->trap_count = 1;
    rules_force_d20(20);
    run(c, "disarm");
    CHECK(t->active && said("No detected trap next to you"), "disarmed a trap two steps away");
    run(c, "disarm wildly");
    CHECK(t->active, "disarmed with a wrong word");
    t->x = CX + 1;
    t->y = CY + 1;
    run(c, "disarm wildly");
    CHECK(t->active && said("Usage: disarm"), "no usage for a wrong word");
    run(c, "disarm");
    rules_force_d20(0);
    CHECK(!t->active && said("SUCCESS"), "the diagonal trap was not disarmed");
    return 0;
}

/* throw: "Throw an item: 5 tiles, 8 for ammunition. It lands where its
 * flight ends." One item of a stack flies; without a direction it goes
 * to the creature in a straight line. */
int test_spec_throw(void) {
    hx_reset_world();
    Client *c = hero(0, "thrower", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int dart = item_named("Dart");
    CHECK(dart >= 0, "no dart");
    give(c, dart, 5);
    run(c, "throw dart");
    CHECK(c->backpack[0].stack_count == 5 && said("give a direction"),
          "threw at nothing without a direction");
    NPC *m = monster(10, "Goblin", FLOOR, CX, CY + 3);
    m->ac = 1;
    run(c, "throw dart");
    CHECK(c->backpack_count == 1 && c->backpack[0].stack_count == 4, "%d darts left",
          c->backpack[0].stack_count);
    CHECK(said("towards the south") && m->hp < 30, "the dart did not fly at the goblin");
    /* The dart lies where the goblin stood, alone. */
    bool on_ground = false;
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &g_game.npcs[i];
        const ItemInstance *loot = npc_loot(&g_game.pools, n);
        if (n->active && n->archetype == ARCH_TREASURE && n->x == CX && n->y == CY + 3 && loot &&
            loot[0].template_idx == dart && loot[0].stack_count == 1) {
            on_ground = true;
        }
    }
    CHECK(on_ground, "the dart is not on the ground, one unit, where it landed");
    run(c, "throw dart w");
    CHECK(c->backpack[0].stack_count == 3 && said("towards the west"), "threw west");
    run(c, "throw 1 east");
    CHECK(c->backpack[0].stack_count == 2 && said("towards the east"), "threw by number");
    return 0;
}

/* status: "Your character sheet: abilities and modifiers, armour class,
 * conditions." The armour class is the one monsters roll against, and
 * counts every protective item; conditions are named. */
int test_spec_status_sheet(void) {
    hx_reset_world();
    Client *c = hero(0, "sheet", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int ac0 = get_player_ac(c);
    CHECK(ac0 == 12, "a naked DEX 14 character has AC %d", ac0); /* 10 + 2 */
    int ring = item_named("Ring of Protection");
    CHECK(ring >= 0, "no ring of protection");
    give(c, ring, 1);
    run(c, "wear 1");
    CHECK(get_player_ac(c) == ac0 + 1, "the ring of protection gives AC %d", get_player_ac(c));
    int armour = item_named("Chain Mail");
    CHECK(armour >= 0, "no chain mail");
    give(c, armour, 1)->ac_bonus = 2; /* a +2 armour from the loot */
    run(c, "wear 1");
    int expected = item_database[armour].ac_base + 2 + 1;
    CHECK(get_player_ac(c) == expected, "a +2 chain mail and the ring: AC %d, not %d",
          get_player_ac(c), expected);
    afflict(c, "Poisoned");
    run(c, "status");
    char ac_text[32];
    snprintf(ac_text, sizeof(ac_text), "AC:%d", expected);
    CHECK(said(ac_text), "the sheet does not show AC %d", expected);
    CHECK(said("Prof:+3"), "a level-5 character has proficiency +3");
    CHECK(said("Poisoned (5 rounds)"), "the condition is not named");
    return 0;
}

/* cast: "Cast a spell you know at the nearest creature, or at the one whose
 * name contains target." */
int test_spec_cast_target(void) {
    hx_reset_world();
    Client *c = hero(0, "aimer", FLOOR, CX, CY);
    CHECK(c != NULL && learn(c, "Magic Missile"), "no caster");
    NPC *near = monster(10, "Goblin", FLOOR, CX + 2, CY);
    NPC *far = monster(11, "Skeleton", FLOOR, CX + 6, CY);
    run(c, "cast magic missile skeleton");
    CHECK(far->hp < 30 && near->hp == 30, "the target was not chosen: %d / %d", near->hp, far->hp);
    CHECK(c->spell_slots[1] == 8, "no slot spent");
    run(c, "cast magic missile dragon");
    CHECK(said("no 'dragon' within") && c->spell_slots[1] == 8, "cast at a creature not there");
    run(c, "cast magic missile");
    CHECK(near->hp < 30, "without a target the nearest one is hit");
    run(c, "cast unknown spell");
    CHECK(said("Spell 'unknown spell' not found"), "a spell that does not exist");
    return 0;
}

/* study: "with the word of your class": a wizard studies, a fighter
 * drills; the word of another class teaches nothing. */
int test_spec_study_word(void) {
    hx_reset_world();
    Client *c = hero(0, "student", 0, CX, CY);
    CHECK(c != NULL, "no player");
    c->class_id = CLASS_WIZARD;
    c->level = 20;
    int book = -1;
    for (int i = 0; i < item_database_size && book < 0; i++) {
        const ItemTemplate *t = &item_database[i];
        if (t->category == ITEM_BOOK && t->book_spell_count > 0 &&
            (t->book_class_mask & (1u << CLASS_WIZARD))) {
            book = i;
        }
    }
    CHECK(book >= 0, "no book for wizards");
    give(c, book, 1);
    const TempleInfo *t = &TEMPLES[0];
    place(c, 0, (t->x0 + t->x1) / 2, (t->y0 + t->y1) / 2);
    run(c, "drill 1");
    CHECK(said("learns with 'study 1', not with 'drill'") && !said("LEARNED"),
          "a wizard learned with the fighters' word");
    run(c, "pray 1");
    CHECK(said("not with 'pray'"), "a wizard learned with the clerics' word");
    run(c, "study 1");
    CHECK(said("*** LEARNED ***"), "the wizards' word taught nothing");
    return 0;
}

/* lore: "What is known about an identified item of the backpack; for a
 * book, its spells" (part of the name works); fill: "Refill a lantern
 * with a flask of oil", the lantern held in a hand too. */
int test_spec_lore_and_fill(void) {
    hx_reset_world();
    Client *c = hero(0, "reader", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int dart = item_named("Dart");
    give(c, dart, 1);
    run(c, "lore dar");
    CHECK(said("[LORE] Dart"), "lore by part of the name");

    int book = -1;
    for (int i = 0; i < item_database_size && book < 0; i++) {
        if (item_database[i].category == ITEM_BOOK && item_database[i].book_spell_count > 0) {
            book = i;
        }
    }
    give(c, book, 1);
    run(c, "lore 2");
    int si = spell_named(item_database[book].book_spell_names[0]);
    CHECK(si >= 0, "the book's first spell is not in the data");
    char line[64];
    snprintf(line, sizeof(line), "learned at level %d", spell_required_level(&spell_database[si]));
    CHECK(said(line), "lore does not say when the first spell is learned (%s)", line);

    c->backpack_count = 0;
    int lantern = item_named("Lantern, Hooded"), oil = item_named("Oil (flask)");
    CHECK(lantern >= 0 && oil >= 0, "no lantern or oil");
    give(c, oil, 2);
    c->slot_hand_l = *give(c, lantern, 1);
    c->backpack_count = 1;
    c->slot_hand_l.durability = 10;
    run(c, "fill lantern");
    CHECK(c->slot_hand_l.durability == item_database[lantern].max_durability &&
              c->backpack[0].stack_count == 1,
          "the lantern in hand was not refilled");
    return 0;
}

/* use: "read a scroll, ... zap a wand": a scroll casts its spell once and
 * is gone, a wand spends a charge; nothing is spent when nothing is hit;
 * rest recharges the wands; a silenced reader cannot read. */
int test_spec_scrolls_and_wands(void) {
    hx_reset_world();
    Client *c = hero(0, "zapper", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int scroll = item_named("Scroll of Magic Missile"), wand = item_named("Wand of Magic Missiles");
    CHECK(scroll >= 0 && wand >= 0, "no scroll or wand");
    CHECK(item_database[scroll].category == ITEM_CONSUMABLE &&
              item_database[scroll].cast_spell_idx == spell_named("Magic Missile"),
          "the scroll does not cast Magic Missile");
    give(c, scroll, 2);
    run(c, "read magic missile");
    CHECK(c->backpack[0].stack_count == 2 && said("no enemy within"), "a scroll read at nothing");
    NPC *m = monster(10, "Goblin", FLOOR, CX + 3, CY);
    run(c, "read magic missile");
    CHECK(m->hp < 30 && c->backpack[0].stack_count == 1, "the scroll did not cast once");
    afflict(c, "Silenced");
    run(c, "r 1");
    CHECK(c->backpack[0].stack_count == 1 && said("silenced"), "read while silenced");
    c->effect_count = 0;
    /* From the belt the scroll is used where it is. */
    run(c, "belt 1");
    CHECK(c->backpack_count == 0 && c->belt[0].template_idx == scroll, "the scroll is not belted");
    m->hp = 30;
    run(c, "read magic missile");
    CHECK(m->hp < 30 && c->belt[0].template_idx == -1 && c->backpack_count == 0,
          "the belted scroll was not used up");

    give(c, wand, 1);
    CHECK(c->backpack[0].current_charges == 7, "a new wand has %d charges",
          c->backpack[0].current_charges);
    m->hp = 30;
    afflict(c, "Silenced");
    run(c, "zap wand");
    c->effect_count = 0;
    CHECK(m->hp < 30 && c->backpack[0].current_charges == 6 && said("6 of 7 charges"),
          "the wand spent no charge");
    c->backpack[0].current_charges = 0;
    m->hp = 30;
    run(c, "z 1");
    CHECK(m->hp == 30 && said("no charges left"), "an empty wand fired");
    run(c, "rest");
    CHECK(c->backpack[0].current_charges == 0, "rested in the dungeon");
    place(c, 0, CX, CY);
    run(c, "rest");
    CHECK(c->backpack[0].current_charges == 7 && said("1 wand recharged"), "no recharge in town");
    return 0;
}

/* talk, list, buy, ...: "the merchant next to you": one step, diagonals
 * included. sell and haggle take part of a name; repair one slot; cure
 * poison only; uncurse lifts the curse on the player too. */
int test_spec_merchant_next_to_you(void) {
    hx_reset_world();
    Client *c = hero(0, "patron", 0, CX, CY);
    CHECK(c != NULL, "no player");
    int dart = item_named("Dart"), armour = item_named("Chain Mail"), helm = -1;
    for (int i = 0; i < item_database_size && helm < 0; i++) {
        if (item_database[i].category == ITEM_HEAD && item_database[i].max_durability > 0) {
            helm = i;
        }
    }
    CHECK(dart >= 0 && armour >= 0 && helm >= 0, "data");
    NPC *m = merchant_next(20, 0, CX + 2, CY, dart);
    static const char *const k_services[] = {"talk",   "list",         "browse 1", "buy 1",
                                             "sell 1", "haggle buy 1", "identify", "cure",
                                             "repair", "uncurse"};
    give(c, armour, 1);
    afflict(c, "Poisoned");
    uint64_t gold = c->gold;
    for (size_t i = 0; i < sizeof(k_services) / sizeof(k_services[0]); i++) {
        run(c, k_services[i]);
        CHECK(!said("Spec Shop") && c->gold == gold && c->backpack_count == 1,
              "'%s' served two steps away", k_services[i]);
    }
    move_npc(m, CX + 1, CY - 1);
    run(c, "talk");
    CHECK(said("[Spec Shop] Welcome!"), "no talk next to the merchant");
    run(c, "list");
    CHECK(said("=== Spec Shop ==="), "no list next to the merchant");

    /* sell and haggle by part of the name. */
    run(c, "haggle sell chain");
    CHECK(said("Current offer for your Chain Mail"), "haggle by part of the name");
    run(c, "sell chain");
    CHECK(c->backpack_count == 0 && c->gold > gold, "sell by part of the name");

    /* cure: the poison goes, blindness stays. */
    afflict(c, "Blinded");
    gold = c->gold;
    run(c, "cure");
    CHECK(!has_effect(c, "Poisoned") && has_effect(c, "Blinded") && c->gold == gold - 50,
          "cure took the wrong conditions");
    c->effect_count = 0;

    /* repair one slot by its name. */
    c->gold = 5000;
    c->slot_body = *give(c, armour, 1);
    c->slot_head = *give(c, helm, 1);
    c->backpack_count = 0;
    c->slot_body.durability = 1;
    c->slot_head.durability = 1;
    run(c, "repair head");
    CHECK(c->slot_head.durability == item_database[helm].max_durability &&
              c->slot_body.durability == 1,
          "repair head: head %d, body %d", c->slot_head.durability, c->slot_body.durability);
    run(c, "repair chain");
    CHECK(c->slot_body.durability == item_database[armour].max_durability,
          "repair by part of the name");
    run(c, "repair boots");
    CHECK(said("not wearing 'boots'"), "repaired what is not worn");

    /* uncurse: the curse on the player alone is worth the rite. */
    afflict(c, "Cursed");
    gold = c->gold;
    run(c, "uncurse");
    CHECK(!has_effect(c, "Cursed") && c->gold == gold - 200 && said("the curse on you lifted"),
          "the Cursed condition stayed");
    return 0;
}

/* trade: "Propose a trade to a player next to you"; offer answers with its
 * usage; cancel calls the trade off for both. */
int test_spec_trade(void) {
    hx_reset_world();
    Client *a = hero(0, "anna", FLOOR, CX, CY);
    Client *b = hero(1, "bruno", FLOOR, CX + 2, CY);
    CHECK(a && b, "no players");
    a->trading_with[0] = b->trading_with[0] = '\0';
    run(a, "trade bruno");
    CHECK(said("not next to you") && a->trading_with[0] == '\0', "a trade two steps away");
    place(b, FLOOR, CX + 1, CY + 1);
    run(a, "trade bruno");
    run(b, "trade anna");
    CHECK(strcmp(a->trading_with, "bruno") == 0 && strcmp(b->trading_with, "anna") == 0,
          "no trade between neighbours");
    run(a, "offer lots");
    CHECK(said("Usage: offer"), "offer without a number is silent");
    run(a, "offer 10");
    run(a, "cancel");
    CHECK(a->trading_with[0] == '\0' && b->trading_with[0] == '\0' && said("Trade cancelled"),
          "cancel left a trade open");
    heard(b);
    CHECK(said("anna canceled the trade"), "the other player was not told");
    run(a, "cancel");
    CHECK(said("not in any trade"), "a second cancel");
    return 0;
}

/* retrieve: "Recover your soul and equipment from your ghost, next to it."
 * The equipment is what the tombstone holds. */
int test_spec_retrieve(void) {
    hx_reset_world();
    remove_with_prefix("state-spec_retrieve/saves", "tombstone_");
    Client *c = hero(0, "revenant", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int dart = item_named("Dart");
    give(c, dart, 4);
    c->gold = 321;
    save_bones(&g_game, c);
    CHECK(c->gold == 0 && c->backpack_count == 0, "the bones kept nothing");
    NPC *ghost = NULL;
    for (int i = 0; i < MAX_NPCS && !ghost; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].is_ghost) {
            ghost = &g_game.npcs[i];
        }
    }
    CHECK(ghost != NULL, "no ghost");
    place(c, FLOOR, CX + 3, CY);
    run(c, "retrieve");
    CHECK(said("No ghost of yours next to you") && ghost->active, "retrieved from afar");
    place(c, FLOOR, CX + 1, CY + 1);
    run(c, "retrieve");
    CHECK(!ghost->active && said("recovered your soul"), "the ghost did not find peace");
    CHECK(c->gold == 321 && c->backpack_count == 1 && c->backpack[0].template_idx == dart &&
              c->backpack[0].stack_count == 4,
          "the equipment did not come back: %lu gold, %d items", (unsigned long)c->gold,
          c->backpack_count);
    CHECK(!file_with_prefix("state-spec_retrieve/saves", "tombstone_revenant_"),
          "the emptied tombstone is still on disk");
    return 0;
}

/* dm_spawn_boss: "Create the boss of a boss floor again, once it is
 * dead"; dm_mapfloor: the map of the DM's own floor; dm_shop: "the
 * merchant within 5 steps". */
int test_spec_dm_commands(void) {
    hx_reset_world();
    Client *c = hero(0, "dungeonmaster", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->is_dm = true;
    /* The centre of floor 10 is rock: the boss stands on the nearest
     * walkable tile. */
    set_tile(10, MAP_CENTER_X, MAP_CENTER_Y, VOXEL_ROCK);
    run(c, "dm_spawn_boss 10");
    NPC *boss = NULL;
    for (int i = 0; i < MAX_NPCS && !boss; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].archetype == ARCH_BOSS &&
            g_game.npcs[i].floor_id == 10) {
            boss = &g_game.npcs[i];
        }
    }
    CHECK(boss != NULL && said("successfully spawned on floor 10"), "no boss");
    CHECK(tile(10, boss->x, boss->y) != VOXEL_ROCK, "the boss stands in the rock");
    run(c, "dm_spawn_boss 10");
    CHECK(said("still lives on floor 10"), "a second boss while the first lives");
    npc_set_active(&g_game, boss, false);
    run(c, "dm_spawn_boss 10");
    CHECK(said("successfully spawned on floor 10"), "no boss again after the first died");

    remove_with_prefix("state-spec_dm_commands/maps", "map_dump_floor_1");
    run(c, "dm_mapfloor");
    CHECK(file_with_prefix("state-spec_dm_commands/maps", "map_dump_floor_1."),
          "dm_mapfloor did not dump the DM's floor");

    int dart = item_named("Dart");
    NPC *m = merchant_next(20, FLOOR, CX + 5, CY, -1);
    run(c, "dm_shop 7 2");
    (void)dart;
    CHECK(said("Added item 7 (qty 2) to Spec Shop") &&
              npc_shop_any(&g_game.pools, m)->item_count == 1,
          "dm_shop missed the merchant 5 steps away");
    move_npc(m, CX + 6, CY);
    run(c, "dm_shop 7 2");
    CHECK(said("No merchant within 5 steps"), "dm_shop reached 6 steps away");
    return 0;
}
