/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The spells of exploration, protection and support in the in-process
 * fixture (see harness_magic.h): one test per family of handlers, with
 * what each must and must not do; and the allies, which fight for their
 * caster and leave.
 */

#include "harness_magic.h"

#include "harness_util.h"

#include "ai.h"
#include "items.h"
#include "player.h"
#include "server_internal.h"
#include "server_move.h"
#include "spells.h"
#include "traps.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static int spell_named(const char *name) {
    for (int i = 0; i < spell_database_size; i++) {
        if (strcmp(spell_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*The caster knows the spell, carries its book, has the class that casts
 * it and every slot.*/
static bool learn(Client *c, const char *name) {
    int si = spell_named(name);
    if (si < 0) {
        return false;
    }
    int book = -1;
    for (int i = 0; i < item_database_size && book < 0; i++) {
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

static void cast(Client *c, const char *name) {
    char line[96];
    snprintf(line, sizeof(line), "cast %s", name);
    run(c, line);
}

static bool has(const Client *c, const char *name) {
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].name && strcmp(c->effects[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void afflict(Client *c, const char *name) {
    c->effects[c->effect_count++] =
        (ActiveEffect){name, EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
}

static NPC *first_ally(void) {
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_game.npcs[i].active && npc_is_ally(&g_game.npcs[i])) {
            return &g_game.npcs[i];
        }
    }
    return NULL;
}

/* Sense shows the creatures and the stairs; find_traps finds, or with a
 * value disarms; shape_earth opens stone but never in town; light gives
 * sight for its rounds. */
int test_magic_exploration(void) {
    hx_reset_world();
    Client *c = hero(0, "seer", FLOOR, CX, CY);
    CHECK(c && learn(c, "Clairvoyance") && learn(c, "Find Traps") && learn(c, "Trap Disarm") &&
              learn(c, "Passwall") && learn(c, "Light"),
          "no caster or missing spells");
    Floor *fl = &g_game.world->floors[FLOOR];
    NPC *m = monster(10, NULL, FLOOR, CX + 5, CY - 5);
    fl->map.data[0][CY + 20][CX] = VOXEL_STAIRS_DOWN;
    cast(c, "Clairvoyance");
    CHECK(said(npc_name(m)) && said("5 cells north-east") && said("Stairs down: 20 cells south"),
          "clairvoyance did not see the goblin or the stairs");

    Trap *t = &fl->traps[fl->trap_count++];
    memset(t, 0, sizeof(*t));
    t->type = TRAP_SPIKES;
    t->x = CX + 2;
    t->y = CY;
    t->active = true;
    t->detection_dc = 30;
    cast(c, "Find Traps");
    CHECK(t->detected && t->active && fl->map.data[0][CY][CX + 2] == VOXEL_TRAP &&
              said("1 trap within 20 cells"),
          "find traps");
    t->x = CX + 9;
    t->detected = false;
    cast(c, "Trap Disarm");
    CHECK(t->active && said("0 traps within 3 cells"), "disarmed a trap out of reach");
    t->x = CX + 2;
    cast(c, "Trap Disarm");
    CHECK(!t->active && fl->map.data[0][CY][CX + 2] == VOXEL_FLOOR && said("disarmed"),
          "the trap is still armed");

    fl->map.data[0][CY][CX + 1] = VOXEL_WALL;
    fl->map.data[0][CY + 2][CX - 2] = VOXEL_ROCK;
    cast(c, "Passwall");
    CHECK(fl->map.data[0][CY][CX + 1] == VOXEL_FLOOR &&
              fl->map.data[0][CY + 2][CX - 2] == VOXEL_FLOOR,
          "the stone did not open");
    place(c, 0, CX, CY);
    g_game.world->floors[0].map.data[0][CY][CX + 1] = VOXEL_WALL;
    cast(c, "Passwall");
    CHECK(g_game.world->floors[0].map.data[0][CY][CX + 1] == VOXEL_WALL &&
              said("wards of the town"),
          "the town's walls opened");

    place(c, FLOOR, CX, CY);
    CHECK(get_vision_radius(c) == 4, "sight %d in the dark", get_vision_radius(c));
    cast(c, "Light");
    CHECK(get_vision_radius(c) == 8 && said("you see 8 cells"), "no light");
    c->effect_count = 0;
    CHECK(get_vision_radius(c) == 4, "the light outlived its effect");
    afflict(c, "Blinded");
    cast(c, "Light");
    CHECK(get_vision_radius(c) == 0, "a light let a blind character see");
    return 0;
}

/* Cleanse keeps invisibility; remove_curse lifts worn curses; nourish
 * feeds; mend restores and removes rust; identify takes one item or all;
 * shelter needs no enemy near; rally tells the party. */
int test_magic_care(void) {
    hx_reset_world();
    Client *c = hero(0, "healer", FLOOR, CX, CY);
    Client *friend = hero(1, "friend", 2, CX, CY);
    CHECK(c && friend, "no players");
    CHECK(learn(c, "Lesser Restoration") && learn(c, "Remove Curse") &&
              learn(c, "Create Food and Water") && learn(c, "Fabricate") && learn(c, "Identify") &&
              learn(c, "Legend Lore") && learn(c, "Tiny Hut") && learn(c, "Telepathic Bond"),
          "missing spells");
    afflict(c, "Poisoned");
    afflict(c, "Invisible");
    cast(c, "Lesser Restoration");
    CHECK(!has(c, "Poisoned") && has(c, "Invisible") && said("1 condition cured"), "cleanse");

    int armour = item_of(ITEM_ARMOR);
    CHECK(armour >= 0, "no armour");
    c->slot_body = *give(c, armour, 1);
    c->backpack_count--;
    c->slot_body.blessing = BLESS_CURSED;
    c->slot_body.durability = 1;
    c->slot_body.quality = QUALITY_RUSTY;
    afflict(c, "Cursed");
    cast(c, "Remove Curse");
    CHECK(c->slot_body.blessing == BLESS_NORMAL && !has(c, "Cursed") && said("2 curses lifted"),
          "remove curse");
    cast(c, "Fabricate");
    CHECK(c->slot_body.durability == item_database[armour].max_durability &&
              c->slot_body.quality == QUALITY_NORMAL,
          "mend: %d", c->slot_body.durability);

    c->hunger_level = 900;
    c->exhaustion_level = 2;
    cast(c, "Create Food and Water");
    CHECK(c->hunger_level == 0 && c->exhaustion_level == 1, "nourish");

    int sword = item_of(ITEM_WEAPON);
    give(c, sword, 1)->is_identified = false;
    give(c, sword, 1)->is_identified = false;
    give(c, sword, 1)->is_identified = false;
    int last = c->backpack_count - 1;
    cast(c, "Identify");
    CHECK(c->backpack[last - 2].is_identified && !c->backpack[last - 1].is_identified &&
              said("1 item identified"),
          "identify took more than one");
    cast(c, "Legend Lore");
    CHECK(c->backpack[last].is_identified && said("2 items identified"), "legend lore");

    c->hp = 5;
    monster(10, NULL, FLOOR, CX + 4, CY);
    cast(c, "Tiny Hut");
    CHECK(c->hp == 5 && said("too close"), "rested next to an enemy");
    monster(10, NULL, FLOOR, CX + 25, CY);
    cast(c, "Tiny Hut");
    CHECK(c->hp == c->max_hp, "no rest");

    snprintf(c->party_leader, sizeof(c->party_leader), "healer");
    snprintf(friend->party_leader, sizeof(friend->party_leader), "healer");
    cast(c, "Telepathic Bond");
    CHECK(said("friend: floor 2"), "the friend is not found");
    heard(friend);
    CHECK(said("healer reaches your mind: floor 1"), "the friend was not told");
    return 0;
}

/* A summoned creature fights for its caster: it kills a monster (the kill
 * is the caster's), never attacks the caster, blocks without being hit,
 * and leaves when its rounds end or its caster leaves the floor. */
int test_magic_allies(void) {
    hx_reset_world();
    Client *c = hero(0, "conjurer", FLOOR, CX, CY);
    CHECK(c && learn(c, "Find Familiar"), "no caster");
    g_game.event.type = 0;
    cast(c, "Find Familiar");
    NPC *ally = first_ally();
    CHECK(ally && said("answers your call") && strncmp(ally->custom_name, "[SUMMONED]", 10) == 0,
          "no familiar");
    ally->hp = ally->max_hp = 1000;
    ally->attack_bonus = 50;
    ally->damage_dice = 1;
    ally->damage_sides = 4;
    ally->respawn_timer = 0;
    ally->ai_ctx.state_timer = 100;
    NPC *m = monster(10, NULL, FLOOR, CX + 4, CY);
    m->respawn_timer = 0;
    m->hp = 2;
    m->xp_reward = 40;
    int xp = c->xp;
    c->hp = c->max_hp = 100000;
    for (int r = 0; r < 60 && m->active; r++) {
        g_game.total_turns = 2000 + r;
        update_world(g_game.clients, &g_game);
        c->hp = c->max_hp;
    }
    CHECK(!m->active && c->xp == xp + 40, "the familiar did not kill for its caster");
    m->respawn_timer = -1; /* out of the way of the rest of the test */

    /* Alone with its caster for many rounds: it never strikes. */
    c->hp = c->max_hp = 60;
    ally->ai_ctx.state_timer = 100;
    for (int r = 0; r < 40; r++) {
        g_game.total_turns = 3000 + r;
        update_world(g_game.clients, &g_game);
    }
    CHECK(c->hp == 60, "the familiar attacked its caster");
    CHECK(abs(ally->x - c->x) <= 2 && abs(ally->y - c->y) <= 2, "the familiar did not follow");

    /* Stepping into it does not attack it. */
    MsgMove mv = {.dx = (int8_t)((ally->x > c->x) - (ally->x < c->x)),
                  .dy = (int8_t)((ally->y > c->y) - (ally->y < c->y))};
    int hp = ally->hp;
    c->last_action_ms = 0;
    place(c, FLOOR, ally->x - mv.dx, ally->y - mv.dy);
    hush(c);
    player_move(c, &mv, g_test_clients, &g_game);
    heard(c);
    CHECK(ally->hp == hp && said("stands beside you"), "the caster attacked the familiar");

    /* Its rounds run out: gone, and never back. */
    ally->ai_ctx.state_timer = 1;
    hush(c);
    g_game.total_turns = 4001;
    update_world(g_game.clients, &g_game);
    heard(c);
    CHECK(!ally->active && said("fades away"), "the familiar outlived its rounds");
    for (int r = 0; r < RESPAWN_TICKS + 5; r++) {
        g_game.total_turns = 5000 + r;
        update_world(g_game.clients, &g_game);
    }
    CHECK(!ally->active, "a summoned creature respawned as a monster");

    /* A dominated monster is hostile again when the spell ends. */
    CHECK(learn(c, "Dominate Monster"), "no Dominate Monster");
    NPC *d = monster(11, NULL, FLOOR, CX + 3, CY);
    d->respawn_timer = 0;
    rules_force_d20(1);
    cast(c, "Dominate Monster");
    rules_force_d20(0);
    CHECK(npc_is_ally(d), "the monster is not dominated");
    d->ai_ctx.state_timer = 1;
    g_game.total_turns = 6001;
    update_world(g_game.clients, &g_game);
    CHECK(d->active && !npc_is_ally(d) && d->custom_name[0] == '\0', "domination did not end");

    /* An ally leaves when its caster leaves the floor. */
    cast(c, "Find Familiar");
    ally = first_ally();
    CHECK(ally != NULL, "no second familiar");
    place(c, 2, CX, CY);
    g_game.total_turns = 6003;
    update_world(g_game.clients, &g_game);
    CHECK(!ally->active, "the familiar stayed without its caster");
    return 0;
}

/* Invisible: the monsters do not see the character; an attack or a
 * hostile spell makes it visible again. */
int test_magic_invisibility(void) {
    hx_reset_world();
    Client *c = hero(0, "ghost", FLOOR, CX, CY);
    CHECK(c && learn(c, "Greater Invisibility") && learn(c, "Fire Bolt"), "missing spells");
    g_game.event.type = 0;
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->respawn_timer = 0;
    cast(c, "Greater Invisibility");
    CHECK(has(c, "Invisible"), "not invisible");
    rules_force_d20(20);
    for (int r = 0; r < 8; r++) { /* the spell lasts 10 rounds */
        g_game.total_turns = 7000 + r;
        update_world(g_game.clients, &g_game);
    }
    CHECK(c->hp == 60, "a monster struck an invisible character");
    cast(c, "Fire Bolt");
    rules_force_d20(0);
    CHECK(!has(c, "Invisible") && said("visible again"), "a spell kept invisibility");
    afflict(c, "Invisible");
    m = monster(10, NULL, FLOOR, c->x + 1, c->y);
    m->hp = 1000;
    MsgMove mv = {.dx = 1, .dy = 0};
    c->last_action_ms = 0;
    hush(c);
    player_move(c, &mv, g_test_clients, &g_game);
    heard(c);
    CHECK(!has(c, "Invisible"), "an attack kept invisibility");
    return 0;
}
