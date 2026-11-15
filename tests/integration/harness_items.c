/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Items, merchants' services, the character sheet and the social commands
 * in the in-process fixture (see harness_items.h).
 */

#include "harness_items.h"

#include "harness_util.h"

#include "classes.h"
#include "items.h"
#include "player.h"
#include "server_cmd.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "spells.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

/* The first item of the data that satisfies the test, or -1. */
static int first_item(bool (*ok)(const ItemTemplate *)) {
    for (int i = 0; i < item_database_size; i++) {
        if (ok(&item_database[i])) {
            return i;
        }
    }
    return -1;
}

static bool is_healing(const ItemTemplate *t) {
    return t->category == ITEM_CONSUMABLE && t->heal_amount > 0 && !strcasestr(t->name, "Ration");
}
static bool is_ration(const ItemTemplate *t) {
    return t->category == ITEM_CONSUMABLE && strcasestr(t->name, "Ration");
}
static bool is_effect_potion(const ItemTemplate *t) {
    return t->category == ITEM_CONSUMABLE && t->heal_amount <= 0;
}
static bool is_lantern(const ItemTemplate *t) {
    return t->category == ITEM_LIGHT_SOURCE && t->light_radius > 6 && t->max_durability > 0 &&
           !strcasestr(t->name, "torch");
}
static bool is_fuel(const ItemTemplate *t) {
    return t->category == ITEM_FUEL;
}
static bool is_book(const ItemTemplate *t) {
    return t->category == ITEM_BOOK && t->book_spell_count > 0 &&
           (t->book_class_mask & (1u << CLASS_WIZARD));
}
static bool is_armour(const ItemTemplate *t) {
    return t->category == ITEM_ARMOR && t->max_durability > 0;
}
static bool is_weapon(const ItemTemplate *t) {
    return t->category == ITEM_WEAPON && t->max_durability > 0;
}

static NPC *merchant(int slot, int floor, int x, int y) {
    NPC *m = &g_game.npcs[slot];
    memset(m, 0, sizeof(*m));
    m->archetype = ARCH_MERCHANT;
    m->entity_id = 9000 + slot;
    m->floor_id = floor;
    m->x = m->spawn_x = x;
    m->y = m->spawn_y = y;
    m->hp = m->max_hp = 100;
    m->respawn_timer = -1;
    MerchantData *shop = npc_shop_new(&g_game.pools, g_game.npcs, MAX_NPCS, m);
    snprintf(shop->shop_name, sizeof(shop->shop_name), "Test Shop");
    npc_set_active(&g_game, m, true);
    g_game.world->floors[floor].entity_grid[y][x] = m->entity_id;
    return m;
}

/* Potions, rations, effect items, fuel, books: what each does, and what
 * cannot be used. */
int test_use_items(void) {
    hx_reset_world();
    Client *c = hero(0, "user", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int heal = first_item(is_healing), ration = first_item(is_ration);
    int effect = first_item(is_effect_potion), lantern = first_item(is_lantern);
    int oil = first_item(is_fuel), sword = first_item(is_weapon);
    CHECK(heal >= 0 && ration >= 0 && effect >= 0 && lantern >= 0 && oil >= 0 && sword >= 0,
          "data: heal %d ration %d effect %d lantern %d oil %d sword %d", heal, ration, effect,
          lantern, oil, sword);

    run(c, "use");
    CHECK(said("Specify the subject"), "no usage");
    run(c, "use 1");
    CHECK(said("Item not found"), "used nothing");

    c->hp = 10;
    give(c, heal, 2);
    run(c, "use 1");
    CHECK(c->hp > 10 && c->backpack[0].stack_count == 1 && said("recovered"), "no healing");

    c->backpack_count = 0;
    c->hunger_level = 3;
    give(c, ration, 1);
    run(c, "eat");
    CHECK(c->hunger_level == 0 && c->backpack_count == 0 && said("You ate"), "no meal");

    give(c, effect, 1)->is_identified = false;
    run(c, "use 1");
    CHECK(said("must first identify") && c->backpack_count == 1, "used an unknown item");
    c->backpack[0].is_identified = true;
    run(c, "use 1");
    CHECK(c->effect_count == 1 && said("Effect active"), "no effect");

    c->backpack_count = 0;
    give(c, sword, 1);
    run(c, "use 1");
    CHECK(said("is not consumable"), "used a sword");
    run(c, "study 1");
    CHECK(said("only for learning from magical books"), "studied a sword");

    /* Oil refills a lantern in a hand; fill does the same by name. */
    c->backpack_count = 0;
    give(c, oil, 3);
    run(c, "use 1");
    CHECK(said("don't have a lantern") && c->backpack[0].stack_count == 3, "oil without lantern");
    c->slot_hand_l = *give(c, lantern, 1);
    c->backpack_count = 1;
    c->slot_hand_l.durability = 1;
    run(c, "use 1");
    CHECK(c->slot_hand_l.durability == item_database[lantern].max_durability &&
              c->backpack[0].stack_count == 2,
          "the lantern was not refilled");
    give(c, lantern, 1)->durability = 3;
    run(c, "fill 2");
    CHECK(c->backpack[1].durability == item_database[lantern].max_durability && said("You filled"),
          "fill did not refill");
    run(c, "fill 2");
    CHECK(said("already full"), "filled a full lantern");
    run(c, "fill 1");
    CHECK(said("not a rechargeable light source"), "filled the oil");
    run(c, "fill nothing");
    CHECK(said("Item to fill not found"), "filled nothing");
    return 0;
}

/* Books: lore shows the spells; a book is studied with the class ritual
 * inside the class temple only. */
int test_books_and_lore(void) {
    hx_reset_world();
    Client *c = hero(0, "scholar", 0, CX, CY);
    CHECK(c != NULL, "no player");
    c->class_id = CLASS_WIZARD;
    int book = first_item(is_book);
    CHECK(book >= 0, "no wizard book");
    give(c, book, 1);
    run(c, "lore");
    CHECK(said("Specify the object"), "no usage");
    run(c, "lore 1");
    CHECK(said("[LORE]") && said("--- Spell Index ---"), "no spell index");
    run(c, "lore 9");
    CHECK(said("Item not found"), "lore of nothing");
    run(c, "use 1");
    CHECK(said("Use 'study'"), "a book used like a potion");
    run(c, "study 1");
    CHECK(said("you must go to floor 0, in Arcane Academy"), "studied outside the temple");

    const TempleInfo *t = &TEMPLES[0];
    CHECK(t->cls == CLASS_WIZARD, "the first temple is not the wizards'");
    place(c, 0, (t->x0 + t->x1) / 2, (t->y0 + t->y1) / 2);
    c->level = 20;
    run(c, "study 1");
    CHECK(said("*** LEARNED ***") && said("new magic(s)"), "nothing learned in the temple");
    run(c, "study 1");
    CHECK(said("already know all the accessible magic"), "learned twice");

    c->class_id = CLASS_FIGHTER;
    run(c, "drill 1");
    CHECK(said("does not include"), "a fighter read a wizard's book");
    c->class_id = CLASS_WIZARD;
    return 0;
}

/* wear, remove by slot name, belt and unbelt, the inventory and the
 * character sheet. */
int test_equipment(void) {
    hx_reset_world();
    Client *c = hero(0, "knight", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int armour = first_item(is_armour), sword = first_item(is_weapon),
        heal = first_item(is_healing);
    CHECK(armour >= 0 && sword >= 0 && heal >= 0, "no armour, weapon or potion");
    give(c, armour, 1);
    give(c, sword, 1);
    give(c, heal, 3);
    run(c, "wear 1");
    CHECK(c->slot_body.template_idx == armour, "the armour is not worn");
    run(c, "wear nothing");
    CHECK(said("Object not found"), "wore nothing");
    run(c, "wear 1"); /* the potions moved to slot 1 */
    CHECK(said("cannot wear"), "wore a potion");

    run(c, "belt 1");
    CHECK(c->belt[0].template_idx == heal && said("moved to belt"), "nothing on the belt");
    run(c, "belt zzz");
    CHECK(said("Item not found"), "belted nothing");
    run(c, "inventory");
    CHECK(said("--- EQUIPPED ---") && said("[BODY   ]") && said("--- ON BELT ---") &&
              said("[SLOT 1]"),
          "the inventory misses something");
    run(c, "unbelt");
    CHECK(c->belt[0].template_idx == -1 && c->backpack_count == 2, "unbelt did nothing");

    run(c, "status");
    CHECK(said("======== knight ========") && said("--- ATTRIBUTES ---") && said("Armor:"),
          "no character sheet");

    run(c, "remove body");
    CHECK(c->slot_body.template_idx == -1 && c->backpack_count == 3, "the armour stayed on");
    run(c, "remove head");
    CHECK(said("not found") || said("Nothing"), "removed an empty slot: %s", hx_out);

    /* A cursed item stays on. */
    int idx = -1;
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == sword) {
            idx = i;
        }
    }
    CHECK(idx >= 0, "no sword in the backpack");
    c->backpack[idx].blessing = BLESS_CURSED;
    char line[32];
    snprintf(line, sizeof(line), "wear %d", idx + 1);
    run(c, line);
    CHECK(c->slot_hand_r.template_idx == sword, "the sword is not wielded");
    run(c, "remove hand_r");
    CHECK(c->slot_hand_r.template_idx == sword, "a cursed sword came off");
    return 0;
}

/* Level up from experience: hit points, slots, points to train; train
 * spends them, and Constitution raises the hit points. */
int test_level_up_and_train(void) {
    hx_reset_world();
    Client *c = hero(0, "climber", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->level = 1;
    c->xp = 0;
    c->max_hp = 10;
    c->class_id = CLASS_WIZARD;
    c->unspent_stat_points = 0;
    memset(c->spell_slots_max, 0, sizeof(c->spell_slots_max));
    c->xp = 1000; /* past level 2 (300) and 3 (900) */
    hush(c);
    check_level_up(c);
    heard(c);
    CHECK(c->level == 3 && c->unspent_stat_points == 6 && c->hp == c->max_hp && c->max_hp > 10,
          "level %d, points %d, HP %d/%d", c->level, c->unspent_stat_points, c->hp, c->max_hp);
    CHECK(said("RISEN TO LEVEL 3") && said("Head to the Temple") && c->needs_study,
          "the level up is not told");
    CHECK(c->spell_slots_max[1] + c->spell_slots_max[2] == 2, "no new slots");

    run(c, "train");
    CHECK(said("Specify a stat"), "no usage");
    run(c, "train luck");
    CHECK(said("Unknown stat"), "trained luck");
    int str = c->str;
    run(c, "train strength");
    CHECK(c->str == str + 1 && c->unspent_stat_points == 5, "no strength");
    c->con = 15;
    int max_hp = c->max_hp;
    run(c, "train con");
    CHECK(c->con == 16 && c->max_hp == max_hp + 3 && said("more resistant"), "CON gave no HP");
    run(c, "train random");
    CHECK(c->unspent_stat_points == 3, "random spent nothing");
    c->unspent_stat_points = 0;
    run(c, "train str");
    CHECK(said("no stat points"), "trained without points");
    run(c, "save");
    CHECK(said("Saved."), "not saved");
    return 0;
}

/* The merchants' services: talk, browse, haggle, identify (by number or
 * by name), cure, repair, uncurse; each where it is offered. */
int test_merchant_services(void) {
    hx_reset_world();
    Client *c = hero(0, "customer", 0, CX, CY);
    CHECK(c != NULL, "no player");
    int armour = first_item(is_armour), heal = first_item(is_healing);
    int sword = first_item(is_weapon);
    CHECK(armour >= 0 && heal >= 0 && sword >= 0, "data");

    run(c, "talk");
    CHECK(said("no merchant within 2 steps"), "talked to nobody");
    run(c, "cure");
    CHECK(said("find one first"), "cured without a merchant");
    run(c, "identify");
    CHECK(said("must be near a merchant"), "identified without a merchant");
    run(c, "repair");
    CHECK(said("must be near the Blacksmith"), "repaired without a smith");
    run(c, "uncurse");
    CHECK(said("must be near the Cleric"), "uncursed without a cleric");

    NPC *m = merchant(20, 0, CX + 1, CY);
    add_item_to_shop(npc_shop_any(&g_game.pools, m), heal, 5);
    run(c, "talk");
    CHECK(said("[Test Shop] Welcome!") && said("=== Test Shop ==="), "the merchant is mute");
    run(c, "browse 1");
    CHECK(said(item_database[heal].name), "browse shows nothing");

    /* Identify: by number, by name, and a name that is not there. */
    give(c, sword, 1)->is_identified = false;
    give(c, heal, 1)->is_identified = false;
    c->gold = 1000;
    run(c, "identify potion");
    CHECK(c->backpack[1].is_identified && !c->backpack[0].is_identified && c->gold == 900,
          "identify by name took the wrong item");
    run(c, "identify zzz");
    CHECK(said("no unidentified items") && c->gold == 900, "identified a name that is not there");
    c->backpack[0].blessing = BLESS_CURSED;
    run(c, "identify 1");
    CHECK(c->backpack[0].is_identified && said("CURSED"), "the curse is not told");
    c->gold = 50;
    c->backpack[0].is_identified = false;
    run(c, "identify");
    CHECK(said("costs 100gp. You don't"), "identified on credit");

    /* Cure: conditions go, the effect of a potion stays. */
    c->gold = 1000;
    c->effect_count = 0;
    run(c, "cure");
    CHECK(said("no affliction") && c->gold == 1000, "paid for nothing");
    c->effects[c->effect_count++] =
        (ActiveEffect){"Poisoned", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
    c->effects[c->effect_count++] =
        (ActiveEffect){"Potion of Heroism", EVENT_ON_TURN_START, MOD_ADDITIVE, 2, 5, false};
    run(c, "cure");
    CHECK(c->effect_count == 1 && strcmp(c->effects[0].name, "Potion of Heroism") == 0 &&
              c->gold == 950,
          "cure: %d effects, %lu gold", c->effect_count, (unsigned long)c->gold);

    /* Repair: 5 gold a point, rust gone. */
    c->slot_body = *give(c, armour, 1);
    c->backpack_count = 2;
    int max = item_database[armour].max_durability;
    c->slot_body.durability = max - 4;
    c->slot_body.quality = QUALITY_RUSTY;
    run(c, "repair");
    CHECK(c->slot_body.durability == max && c->slot_body.quality == QUALITY_NORMAL &&
              c->gold == 930,
          "repair: %d/%d, %lu gold", c->slot_body.durability, max, (unsigned long)c->gold);
    run(c, "repair");
    CHECK(said("nothing to repair"), "repaired twice");

    /* Uncurse: 200 gold, only with something cursed. */
    run(c, "uncurse");
    CHECK(said("no cursed object") && c->gold == 930, "paid to uncurse nothing");
    c->slot_body.blessing = BLESS_CURSED;
    run(c, "uncurse");
    CHECK(c->slot_body.blessing == BLESS_NORMAL && c->gold == 730 && said("1 item(s) purified"),
          "no uncurse");

    /* Haggle: a price is offered, then applied by buy. */
    run(c, "haggle buy 1");
    CHECK(said("Current offer for") && c->pending_trade_merchant_id == m->entity_id, "no haggling");
    run(c, "haggle steal 1");
    CHECK(said("Usage: haggle"), "haggle without buy or sell");

    place(c, FLOOR, CX, CY);
    run(c, "repair");
    CHECK(said("only found on floor 0"), "a smith in the dungeon");
    run(c, "uncurse");
    CHECK(said("only on floor 0"), "a cleric in the dungeon");
    return 0;
}

/* Party, chat, private messages, the block list, who, the leaderboard,
 * the event. */
int test_social(void) {
    hx_reset_world();
    Client *a = hero(0, "alice", FLOOR, CX, CY);
    Client *b = hero(1, "bob", FLOOR, CX + 3, CY);
    Client *far = hero(2, "carol", FLOOR, CX + 40, CY);
    CHECK(a && b && far, "no players");
    a->pending_invite[0] = b->pending_invite[0] = '\0';
    a->blocked_count = b->blocked_count = 0;

    run(a, "invite dave");
    CHECK(said("not found online"), "invited nobody");
    run(b, "join");
    CHECK(said("No pending invitations"), "joined nothing");
    run(a, "invite bob");
    heard(b);
    CHECK(said("alice has invited you"), "bob was not told");
    run(b, "join");
    CHECK(strcmp(b->party_leader, "alice") == 0 && strcmp(a->party_leader, "alice") == 0,
          "no party");
    run(a, "party");
    CHECK(said("PARTY MEMBERS") && said("- bob"), "the party list misses bob");
    run(b, "leave");
    heard(a);
    CHECK(b->party_leader[0] == '\0' && said("bob has left"), "leaving is not told");
    run(a, "leave");
    CHECK(a->party_leader[0] == '\0', "the leader stayed");
    run(a, "party");
    CHECK(said("not in any party"), "a party of nobody");

    a->last_msg_ms = b->last_msg_ms = 0;
    run(a, "say hello there");
    CHECK(said("[1 nearby]"), "say reached %s", hx_out);
    heard(b);
    CHECK(said("[CHAT] alice: hello there"), "bob did not hear");
    heard(far);
    CHECK(!said("hello there"), "carol heard from 40 steps");
    run(a, "say again");
    CHECK(said("typing too fast"), "no rate limit");
    a->last_msg_ms = 0;
    run(a, "msg carol psst");
    heard(far);
    CHECK(said("[MSG] alice: psst"), "no private message");
    a->last_msg_ms = 0;
    run(a, "msg alice hi");
    CHECK(said("cannot message yourself"), "a message to oneself");
    run(a, "msg dave hi");
    CHECK(said("not online"), "a message to nobody");

    run(far, "block alice");
    CHECK(far->blocked_count == 1 && said("has been blocked"), "no block");
    run(far, "block alice");
    CHECK(said("already blocked"), "blocked twice");
    run(far, "block carol");
    CHECK(said("cannot block yourself"), "blocked oneself");
    a->last_msg_ms = 0;
    run(a, "msg carol hello");
    CHECK(said("not delivered"), "a blocked message was delivered");
    run(far, "blocks");
    CHECK(said("- alice"), "the block list misses alice");
    run(far, "unblock alice");
    CHECK(far->blocked_count == 0, "no unblock");
    run(far, "unblock alice");
    CHECK(said("not in your block list"), "unblocked twice");
    run(far, "blocks");
    CHECK(said("block list is empty"), "an empty list");

    run(a, "who");
    CHECK(said("3 player(s) online") && said("alice (floor 1) (you)"), "who: %s", hx_out);
    run(a, "top");
    CHECK(said("LEADERBOARD") && said("alice"), "no leaderboard");
    g_game.event.type = 0;
    run(a, "event");
    CHECK(said("No global events"), "an event from nowhere");
    g_game.event.type = 1;
    g_game.event.floor_id = 3;
    g_game.event.goal = 5;
    g_game.event.time_left = 600;
    run(a, "event");
    g_game.event.type = 0;
    CHECK(said("Invasion of Plane 3") && said("Time left: 2 min"), "event: %s", hx_out);
    return 0;
}
