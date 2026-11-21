/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The dungeon itself in the in-process fixture (see harness_world.h):
 * every kind of trap, finding and disarming them, looking around, the
 * services of the town, mining, and what a step meets.
 */

#include "harness_world.h"

#include "harness_util.h"

#include "items.h"
#include "map.h"
#include "server_internal.h"
#include "server_move.h"
#include "server_traps.h"
#include "traps.h"

#include <stdlib.h>
#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static Trap *trap_at(int floor, TrapType type, int x, int y) {
    Floor *fl = &g_game.world->floors[floor];
    Trap *t = &fl->traps[fl->trap_count++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->x = x;
    t->y = y;
    t->floor_id = floor;
    t->detection_dc = 30; /* not found by passive perception */
    t->save_dc = 15;
    t->damage_dice = 1;
    t->damage_sides = 4;
    t->active = true;
    return t;
}

static bool has(const Client *c, const char *effect) {
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].name && strcmp(c->effects[i].name, effect) == 0) {
            return true;
        }
    }
    return false;
}

/* Every trap, its save failed: it hurts, inflicts its condition, changes
 * the world where it should, and disarms (but the teleport and the
 * mirrors, which stay). */
int test_every_trap(void) {
    static const struct {
        TrapType type;
        const char *effect; /* NULL: none */
    } k_effects[] = {
        {TRAP_POISON_NEEDLE, "Poisoned"},
        {TRAP_GAS, "Poisoned"},
        {TRAP_GAS_VEIN, "Poisoned"},
        {TRAP_WEB, "Restrained"},
        {TRAP_BLADE, "Bleeding"},
        {TRAP_SILENCE, "Silenced"},
        {TRAP_QUICKSAND, "Prone"},
        {TRAP_DARKNESS, "Blinded"},
        {TRAP_PARALYZING_SPORES, "Paralyzed"},
        {TRAP_SLEEP_GAS, "Unconscious"},
        {TRAP_ANTIMAGIC_ZONE, "Silenced"},
        {TRAP_INVERSION_RUNE, "Confused"},
        {TRAP_HUNGER_CURSE, "Bleeding"},
        {TRAP_DART_WALL, "Poisoned"},
        {TRAP_SPRING_SPEAR, "Restrained"},
    };
    for (int type = 0; type < TRAP_COUNT; type++) {
        hx_reset_world();
        Client *c = hero(0, "trapper", FLOOR, CX, CY);
        CHECK(c != NULL, "no player");
        /* Loot and a monster nearby, for the swaps: only the monster counts. */
        NPC *gold = &g_game.npcs[5];
        memset(gold, 0, sizeof(*gold));
        gold->archetype = ARCH_GOLD;
        gold->x = CX + 2;
        gold->y = CY;
        gold->floor_id = FLOOR;
        gold->entity_id = 9005;
        gold->respawn_timer = -1;
        npc_set_active(&g_game, gold, true);
        NPC *m = monster(10, NULL, FLOOR, CX + 3, CY);
        Trap *t;
        if (type == TRAP_DART_WALL || type == TRAP_SPRING_SPEAR) {
            /* In the wall north of the player, firing south onto it. */
            t = trap_at(FLOOR, (TrapType)type, CX, CY - 1);
            t->wall_dir = 2;
            g_game.world->floors[FLOOR].map.data[0][CY - 1][CX] = VOXEL_WALL;
        } else {
            t = trap_at(FLOOR, (TrapType)type, CX, CY);
        }
        rules_force_d20(1);
        check_traps(c, &g_game);
        heard(c);
        rules_force_d20(0);
        CHECK(c->hp < 60 || type == TRAP_BODY_SWAP, "trap %d did no damage", type);
        for (size_t i = 0; i < sizeof(k_effects) / sizeof(k_effects[0]); i++) {
            if (k_effects[i].type == (TrapType)type) {
                CHECK(has(c, k_effects[i].effect), "trap %d: no %s", type, k_effects[i].effect);
            }
        }
        bool stays = type == TRAP_TELEPORT || type == TRAP_FAKE_DOOR ||
                     type == TRAP_ILLUSION_MIRROR || type == TRAP_SWAP_TELEPORT;
        CHECK(t->active == stays, "trap %d: active %d after firing", type, t->active);
        const Floor *fl = &g_game.world->floors[FLOOR];
        switch (type) {
        case TRAP_FALLING_FLOOR:
            CHECK(c->floor_id == FLOOR + 1, "the floor did not give way");
            break;
        case TRAP_FLOOD:
            CHECK(fl->map.data[0][CY + 3][CX + 3] == VOXEL_WATER, "no flood");
            break;
        case TRAP_STONE_TOMB:
            CHECK(fl->map.data[0][CY][CX + 1] == VOXEL_WALL &&
                      fl->map.data[0][CY - 1][CX - 1] == VOXEL_WALL,
                  "no stone tomb");
            break;
        case TRAP_CEILING_COLLAPSE: {
            int rocks = 0;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    rocks += fl->map.data[0][CY + dy][CX + dx] == VOXEL_ROCK;
                }
            }
            CHECK(rocks > 0, "no rock fell");
            break;
        }
        case TRAP_SWAP_TELEPORT:
            CHECK(c->x == CX + 3 && m->x == CX, "no swap with the monster: at %d", c->x);
            CHECK(gold->x == CX + 2, "the gold pile moved");
            break;
        case TRAP_BODY_SWAP:
            CHECK(c->hp == 30 && m->hp < 60 && m->hp >= 56, "hit points %d / %d", c->hp, m->hp);
            break;
        case TRAP_TELEPORT:
        case TRAP_FAKE_DOOR:
            CHECK(c->x != CX || c->y != CY, "trap %d did not teleport", type);
            break;
        default:
            break;
        }
    }

    /* A saved floor trap does nothing but disarm. */
    hx_reset_world();
    Client *c = hero(0, "trapper", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    trap_at(FLOOR, TRAP_PIT, CX, CY);
    rules_force_d20(20);
    check_traps(c, &g_game);
    heard(c);
    CHECK(c->hp == 60 && said("You avoid the worst"), "a saved pit hurt");

    /* A trap can kill: the town, a tombstone. */
    Trap *pit = trap_at(FLOOR, TRAP_PIT, CX, CY);
    pit->damage_dice = 10;
    c->hp = 1;
    rules_force_d20(1);
    check_traps(c, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(c->floor_id == 0 && c->hp == c->max_hp && said("You died from a trap"),
          "no death by trap");
    CHECK(g_game.tombstones[0].active, "no tombstone after a death by trap");

    /* A swap with nothing alive nearby says so. */
    hx_reset_world();
    c = hero(0, "trapper", FLOOR, CX, CY);
    trap_at(FLOOR, TRAP_SWAP_TELEPORT, CX, CY);
    rules_force_d20(1);
    check_traps(c, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(said("finds no target nearby"), "a swap without a monster");
    return 0;
}

/* Passive perception finds a trap close by; 'search' finds a hidden one
 * (the d20); 'disarm' works only on a trap found, gives XP, gold when
 * salvaging, and fires the trap on a fumble. */
int test_search_and_disarm(void) {
    hx_reset_world();
    Client *c = hero(0, "rogue", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->wis = 18; /* passive 14 */
    Trap *easy = trap_at(FLOOR, TRAP_SPIKES, CX + 2, CY);
    easy->detection_dc = 12;
    check_traps(c, &g_game);
    heard(c);
    CHECK(easy->detected && said("something suspicious"), "passive perception missed it");
    CHECK(g_game.world->floors[FLOOR].map.data[0][CY][CX + 2] == VOXEL_TRAP, "not on the map");

    hx_reset_world();
    c = hero(0, "rogue", FLOOR, CX, CY);
    Trap *t = trap_at(FLOOR, TRAP_PIT, CX + 1, CY);
    t->detection_dc = 15;
    run(c, "disarm");
    CHECK(said("No detected trap next to you"), "an unseen trap was disarmed");
    rules_force_d20(2);
    run(c, "search");
    CHECK(!t->detected && said("don't notice anything"), "a bad search found it");
    rules_force_d20(20);
    run(c, "search");
    CHECK(t->detected && said("discovered a hidden trap"), "a good search missed it");
    CHECK(g_game.world->floors[FLOOR].map.data[0][CY][CX + 1] == VOXEL_TRAP, "not on the map");

    /* A wall trap is found from the tile it fires onto. */
    Trap *dart = trap_at(FLOOR, TRAP_DART_WALL, CX - 1, CY - 2);
    dart->wall_dir = 2; /* fires onto (CX-1, CY-1), next to the player */
    dart->detection_dc = 15;
    run(c, "search");
    CHECK(dart->detected, "the dart trap was not found");

    int xp = c->xp;
    uint64_t gold = c->gold;
    run(c, "disarm salvage");
    CHECK(!t->active && c->xp > xp && c->gold > gold && said("CRITICAL SUCCESS"),
          "salvaging failed");
    CHECK(said("extract valuable materials"), "no salvage");
    CHECK(g_game.world->floors[FLOOR].map.data[0][CY][CX + 1] == VOXEL_FLOOR, "trap still drawn");

    /* A fumble fires the trap on the player. */
    Trap *spikes = trap_at(FLOOR, TRAP_SPIKES, CX, CY + 1);
    spikes->detected = true;
    rules_force_d20(1);
    run(c, "disarm");
    CHECK(said("CATASTROPHIC FAILURE") && c->hp < 60, "a fumble did not fire the trap");
    CHECK(c->x == CX && c->y == CY, "the fumble moved the player");

    /* Arcane disarming spends a first-level slot for +4. */
    Trap *rune = trap_at(FLOOR, TRAP_CURSE_RUNE, CX + 1, CY + 1);
    rune->detected = true;
    c->spell_slots[1] = 2;
    rules_force_d20(10);
    run(c, "disarm magic");
    rules_force_d20(0);
    CHECK(c->spell_slots[1] == 1 && said("Arcane Dissipation") && said("Focus +4"),
          "no arcane disarm");
    return 0;
}

/* look: what is in each direction and under the feet; examine: the nine
 * tiles, who stands there, whether a tool is at hand. */
int test_look_and_examine(void) {
    hx_reset_world();
    Client *c = hero(0, "watcher", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, "Goblin", FLOOR, CX, CY - 2);
    CHECK(m->template != NULL, "no goblin");
    g_game.world->floors[FLOOR].map.data[0][CY][CX + 2] = VOXEL_STAIRS_DOWN;
    run(c, "look");
    CHECK(said("[UNDER FEET] Position (60, 60)"), "no position");
    CHECK(said("| N      |") && said(m->template->name), "the goblin north is not seen");
    char hp[48];
    snprintf(hp, sizeof(hp), "%s HP:%d/%d (2p)", m->template->name, m->hp, m->max_hp);
    CHECK(said(hp), "the goblin's hit points are not shown (%s)", hp);
    CHECK(said("| SW     | Free"), "an empty sector is not free");

    /* What shows the same is one entry, how many and the nearest; the
     * nearest entries come first. */
    static const int k_crystal[3][2] = {{-2, 2}, {-3, 2}, {-2, 3}}; /* 3, 4 and 4 tiles */
    for (int k = 0; k < 3; k++) {
        g_game.world->floors[FLOOR].map.data[0][CY + k_crystal[k][1]][CX + k_crystal[k][0]] =
            VOXEL_CRYSTAL_BLUE;
    }
    g_game.world->floors[FLOOR].map.data[0][CY + 1][CX - 1] = VOXEL_TRAP;
    run(c, "look");
    CHECK(said("| SW     | Trap (1p), Crystal x3 (3p)"), "the crystals are not grouped");

    c->effects[c->effect_count++] =
        (ActiveEffect){"Blinded", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    run(c, "look");
    c->effect_count = 0;
    CHECK(said("can't see anything"), "a blind character saw");

    NPC *close = monster(11, "Goblin", FLOOR, CX + 1, CY);
    close->hp = 7;
    g_game.world->floors[FLOOR].map.data[0][CY - 1][CX] = VOXEL_GOLD_VEIN;
    run(c, "examine");
    CHECK(said("| TILE   | GROUND / WALL"), "no table header");
    CHECK(said("(HP:7/30)"), "the neighbour is not listed");
    CHECK(said("VERY RICH"), "the gold vein is not rated");
    CHECK(said("No pickaxes or shovels"), "a tool that is not there");
    /* A war pick is a weapon, not a pickaxe. */
    give(c, item_named("War pick"), 1);
    run(c, "examine");
    CHECK(said("No pickaxes or shovels"), "a war pick digs");
    int pick = item_named("Miner's");
    CHECK(pick >= 0, "no pick in the data");
    give(c, pick, 1);
    run(c, "examine");
    CHECK(said("You have the right tool"), "the pick in the backpack is not seen");
    return 0;
}

/* The fountain heals and the statue restores, near them in town only;
 * rest is for the town; prayer trades 100 XP for a quarter of the HP. */
int test_town_services(void) {
    hx_reset_world();
    Client *c = hero(0, "pilgrim", 0, CX, CY);
    CHECK(c != NULL, "no player");
    Floor *town = &g_game.world->floors[0];
    c->hp = 10;
    run(c, "fountain");
    CHECK(c->hp == 10 && said("not near any healing fountains"), "a fountain far away");
    town->map.data[0][CY + 2][CX] = VOXEL_WATER;
    run(c, "fountain");
    CHECK(c->hp == c->max_hp && said("sacred fountain"), "the fountain did not heal");

    c->spell_slots_max[1] = 4;
    c->spell_slots[1] = 0;
    c->hunger_level = 3;
    run(c, "statue");
    CHECK(c->spell_slots[1] == 0 && said("not near any crystal statues"), "a statue far away");
    town->map.data[0][CY - 3][CX + 3] = VOXEL_CRYSTAL_BLUE;
    run(c, "statue");
    CHECK(c->spell_slots[1] == 4 && c->hunger_level == 0, "the statue did not restore");

    c->hp = 5;
    c->spell_slots[1] = 0;
    run(c, "rest");
    CHECK(c->hp == c->max_hp && c->spell_slots[1] == 4 && said("Rested"), "no rest in town");

    place(c, FLOOR, CX, CY);
    run(c, "fountain");
    CHECK(said("only found in the Magic Sanctuary"), "a fountain in the dungeon");
    run(c, "statue");
    CHECK(said("only found in the Magic Sanctuary"), "a statue in the dungeon");

    c->hp = 20;
    c->xp = 50;
    run(c, "pray");
    CHECK(c->hp == 20 && said("Gods do not respond"), "a prayer without XP");
    c->xp = 150;
    run(c, "pray");
    CHECK(c->hp == 35 && c->xp == 50, "prayer: HP %d, XP %d", c->hp, c->xp);
    return 0;
}

/* tunnel: gold, crystals that grow back, mushrooms, rock, the ground;
 * nothing without a pickaxe or a shovel; nothing in town. */
int test_mining(void) {
    hx_reset_world();
    Client *c = hero(0, "miner", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    Floor *fl = &g_game.world->floors[FLOOR];
    fl->map.data[0][CY][CX + 1] = VOXEL_GOLD_VEIN;
    fl->map.data[0][CY][CX - 1] = VOXEL_CRYSTAL_RED;
    fl->map.data[0][CY - 1][CX] = VOXEL_MUSHROOM_GLOW;
    fl->map.data[0][CY + 1][CX] = VOXEL_ROCK;

    run(c, "tunnel");
    CHECK(said("Use: tunnel"), "no usage");
    run(c, "tunnel x");
    CHECK(said("Invalid direction"), "a wrong direction");
    run(c, "tunnel e");
    CHECK(fl->map.data[0][CY][CX + 1] == VOXEL_GOLD_VEIN && said("need a pickaxe or a shovel"),
          "gold mined bare-handed");
    run(c, "tunnel d");
    CHECK(fl->map.data[0][CY][CX] == VOXEL_FLOOR && said("need a pickaxe"), "dug bare-handed");
    run(c, "tunnel s");
    CHECK(fl->map.data[0][CY + 1][CX] == VOXEL_ROCK && said("need a pickaxe"), "rock bare-handed");
    run(c, "tunnel north-east");
    CHECK(said("Invalid direction"), "'north-east' read as north");
    /* A war pick is a weapon. */
    give(c, item_named("War pick"), 1);
    run(c, "tunnel e");
    CHECK(fl->map.data[0][CY][CX + 1] == VOXEL_GOLD_VEIN, "a war pick mined gold");
    c->backpack_count = 0;

    int shovel = item_named("Shovel");
    CHECK(shovel >= 0, "no shovel in the data");
    give(c, shovel, 1);
    c->hp = 40;
    run(c, "tunnel north");
    CHECK(c->hp == 55 && fl->map.data[0][CY - 1][CX] == VOXEL_FLOOR, "mushrooms: HP %d", c->hp);
    c->backpack_count = 0;

    int pick = item_named("Miner's");
    CHECK(pick >= 0, "no pick in the data");
    give(c, pick, 1);
    uint64_t gold = c->gold;
    run(c, "tunnel e");
    CHECK(fl->map.data[0][CY][CX + 1] == VOXEL_FLOOR && c->gold > gold && said("pure gold"),
          "no gold");
    gold = c->gold;
    int respawns = fl->crystal_respawn_count;
    run(c, "tunnel w");
    CHECK(c->gold > gold && fl->crystal_respawn_count == respawns + 1 &&
              fl->crystal_respawns[respawns].type == VOXEL_CRYSTAL_RED,
          "the crystal will not grow back");
    run(c, "tunnel s");
    CHECK(fl->map.data[0][CY + 1][CX] == VOXEL_FLOOR, "the rock did not open");
    run(c, "tunnel d");
    CHECK(fl->map.data[0][CY][CX] == VOXEL_COBBLE, "the ground was not dug");
    run(c, "tunnel s");
    CHECK(said("nothing to dig"), "dug the floor sideways");

    place(c, 0, CX, CY);
    g_game.world->floors[0].map.data[0][CY][CX + 1] = VOXEL_CRYSTAL_BLUE;
    run(c, "tunnel e");
    CHECK(g_game.world->floors[0].map.data[0][CY][CX + 1] == VOXEL_CRYSTAL_BLUE &&
              said("The town is not a mine"),
          "a statue of the town was mined");
    return 0;
}

static void step(Client *c, int dx, int dy) {
    MsgMove mv = {.dx = (int8_t)dx, .dy = (int8_t)dy};
    c->last_action_ms = 0;
    hush(c);
    player_move(c, &mv, g_test_clients, &g_game);
    heard(c);
}

/* What a step meets: a wall, a monster (an attack), another player, a
 * chest, an item on the ground, the stairs (sealed by a living boss),
 * ice; and what stops or hurts a step. */
int test_steps(void) {
    hx_reset_world();
    Client *c = hero(0, "walker", FLOOR, CX, CY);
    Client *o = hero(1, "other", FLOOR, CX, CY + 1);
    CHECK(c && o, "no players");
    Floor *fl = &g_game.world->floors[FLOOR];

    fl->map.data[0][CY - 1][CX] = VOXEL_WALL;
    step(c, 0, -1);
    CHECK(c->y == CY, "walked into a wall");
    step(c, 0, 1);
    CHECK(c->y == CY && said("You see other"), "walked through a player");

    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    rules_force_d20(20);
    step(c, 1, 0);
    rules_force_d20(0);
    CHECK(c->x == CX && m->hp < 30, "a step into a monster is not an attack");

    NPC *chest = &g_game.npcs[11];
    memset(chest, 0, sizeof(*chest));
    chest->archetype = ARCH_TREASURE;
    chest->x = CX - 1;
    chest->y = CY;
    chest->floor_id = FLOOR;
    chest->entity_id = 9011;
    chest->respawn_timer = -1;
    int dagger = item_named("Dagger");
    CHECK(dagger >= 0, "no dagger");
    npc_set_active(&g_game, chest, true);
    ItemInstance *in_chest = npc_loot_new(&g_game.pools, g_game.npcs, MAX_NPCS, chest);
    CHECK(in_chest != NULL, "no loot for the chest");
    in_chest[0].template_idx = dagger;
    in_chest[0].stack_count = 1;
    fl->entity_grid[CY][CX - 1] = chest->entity_id;
    step(c, -1, 0);
    CHECK(c->x == CX - 1 && c->backpack_count == 1 && said("You picked up"), "no pickup");

    /* Bleeding: every step costs 2 HP. */
    c->effects[c->effect_count++] =
        (ActiveEffect){"Bleeding", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
    int hp = c->hp;
    step(c, -1, 0);
    CHECK(c->hp == hp - 2 && said("reopens your wounds"), "bleeding did not hurt");
    c->effect_count = 0;
    c->effects[c->effect_count++] =
        (ActiveEffect){"Paralyzed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
    int x = c->x;
    step(c, -1, 0);
    CHECK(c->x == x && said("can't move"), "a paralyzed character moved");
    c->effect_count = 0;

    fl->map.data[0][c->y][c->x - 1] = VOXEL_ICE;
    step(c, -1, 0);
    CHECK(has(c, "Prone") && said("slippery"), "no slip on ice");
    c->effect_count = 0;

    /* Stairs down lead to the up-stairs of the floor below, and back. */
    g_game.world->floors[FLOOR + 1].map.data[0][CY + 5][CX + 5] = VOXEL_STAIRS_UP;
    fl->map.data[0][c->y][c->x - 1] = VOXEL_STAIRS_DOWN;
    step(c, -1, 0);
    CHECK(c->floor_id == FLOOR + 1 && c->x == CX + 5 && c->y == CY + 5, "no descent");
    CHECK(c->max_floor_explored >= FLOOR + 1, "the floor is not remembered");

    /* The stairs of a boss floor are sealed while its boss lives. */
    place(c, 10, CX, CY);
    c->bosses_defeated = 0;
    NPC *boss = monster(12, NULL, 10, CX + 9, CY);
    boss->archetype = ARCH_BOSS;
    g_game.world->floors[10].map.data[0][CY][CX + 1] = VOXEL_STAIRS_DOWN;
    step(c, 1, 0);
    CHECK(c->floor_id == 10 && said("seals the stairs"), "past a living boss");
    c->bosses_defeated = 1;
    place(c, 10, CX, CY);
    step(c, 1, 0);
    CHECK(c->floor_id == 11, "a defeated boss still seals the stairs");
    return 0;
}
