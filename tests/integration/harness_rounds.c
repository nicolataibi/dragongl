/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The rounds of the world in the in-process fixture (see harness_rounds.h):
 * what a round does to a character and to the monsters, what comes back,
 * the global event, the monsters' minds; and the DM's reports.
 */

#include "harness_rounds.h"

#include "harness_util.h"

#include "ai.h"
#include "classes.h"
#include "items.h"
#include "player.h"
#include "server_combat.h"
#include "server_internal.h"
#include "server_spawn.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static bool has(const ActiveEffect *fx, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (fx[i].name && strcmp(fx[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void afflict(Client *c, const char *name, int rounds) {
    c->effects[c->effect_count++] =
        (ActiveEffect){name, EVENT_ON_TURN_START, MOD_ADDITIVE, 0, rounds, false};
}

static void round_at(int turn) {
    g_game.total_turns = turn - 1;
    update_world(g_game.clients, &g_game);
}

/* A round of a character: the light burns, hunger grows and starves,
 * poison and fire hurt (water puts the fire out), a save ends paralysis,
 * a periodic death sends home. Hunger is for the dungeon only. */
int test_player_rounds(void) {
    hx_reset_world();
    Client *c = hero(0, "sufferer", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    g_game.event.type = 0;
    int torch = item_of(ITEM_LIGHT_SOURCE);
    CHECK(torch >= 0, "no light source");
    c->slot_hand_l = *give(c, torch, 1);
    c->backpack_count = 0;
    c->slot_hand_l.durability = 1;
    c->hunger_level = HUNGER_MAX - 1;
    c->exhaustion_level = 0;
    hush(c);
    round_at(99);
    heard(c);
    CHECK(c->slot_hand_l.durability == 0 && said("light source has gone out"), "the light burns");
    CHECK(c->hp == 60, "starving on an odd round");
    hush(c);
    round_at(100);
    heard(c);
    CHECK(c->hp == 58 && c->exhaustion_level == 1 && said("starving") && said("Exhaustion"),
          "hunger: HP %d, exhaustion %d", c->hp, c->exhaustion_level);
    c->hunger_level = 0;

    afflict(c, "Poisoned", 5);
    hush(c);
    round_at(101);
    heard(c);
    CHECK(c->hp == 57 && said("poison coursing"), "poison: HP %d", c->hp);
    c->effect_count = 0;

    afflict(c, "Burning", 5);
    rules_force_d20(1); /* no save puts the fire out */
    round_at(103);
    CHECK(c->hp == 55, "fire: HP %d", c->hp);
    g_game.world->floors[FLOOR].map.data[0][CY][CX] = VOXEL_WATER;
    hush(c);
    round_at(105);
    heard(c);
    rules_force_d20(0);
    CHECK(c->hp == 55 && c->effect_count == 0 && said("Water extinguishes"), "the water");
    g_game.world->floors[FLOOR].map.data[0][CY][CX] = VOXEL_FLOOR;

    afflict(c, "Paralyzed", 5);
    rules_force_d20(20);
    hush(c);
    round_at(107);
    heard(c);
    rules_force_d20(0);
    CHECK(c->effect_count == 0 && said("recovered from the Paralyzed"), "no save");

    afflict(c, "Invisible", 5);
    hush(c);
    round_at(110);
    heard(c);
    CHECK(said("currently invisible"), "invisibility is not told");
    c->effect_count = 0;

    afflict(c, "Bleeding", 5);
    c->hp = 1;
    hush(c);
    round_at(111);
    heard(c);
    CHECK(c->floor_id == 0 && c->hp == c->max_hp && said("You died!"), "no death by bleeding");
    CHECK(g_game.tombstones[0].active, "no bones");

    int hunger = c->hunger_level;
    round_at(113);
    CHECK(c->hunger_level == hunger, "hunger in town");
    return 0;
}

/* The invasion: it starts every 1500 rounds, rewards the defenders of its
 * floor when the goal is met, ends when time runs out. */
int test_global_event(void) {
    hx_reset_world();
    Client *c = hero(0, "defender", FLOOR, CX, CY);
    Client *o = hero(1, "elsewhere", 2, CX, CY);
    CHECK(c && o, "no players");
    g_game.event.type = 0;
    hush(c);
    round_at(1500);
    heard(c);
    CHECK(g_game.event.type == 1 && g_game.event.goal == 5 && said("Invasion on floor"),
          "no invasion");
    g_game.event.floor_id = FLOOR;
    g_game.event.progress = 5;
    uint64_t gold = c->gold, other = o->gold;
    hush(c);
    round_at(1501);
    heard(c);
    CHECK(g_game.event.type == 0 && c->gold == gold + 500 && o->gold == other &&
              said("has been stopped"),
          "no victory");

    g_game.event.type = 1;
    g_game.event.progress = 0;
    g_game.event.time_left = 1;
    hush(c);
    round_at(1503);
    heard(c);
    CHECK(g_game.event.type == 0 && said("Time is up"), "the event never ends");
    return 0;
}

/* Traps re-arm, crystals grow back; a monster dies of its wounds, saves
 * against paralysis, loses heart; the dead come back at their spawn point,
 * later if it is taken; an empty floor gets one back, never a ghost. */
int test_world_respawns(void) {
    hx_reset_world();
    Client *c = hero(0, "witness", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    g_game.event.type = 0;
    Floor *fl = &g_game.world->floors[FLOOR];
    Trap *t = &fl->traps[fl->trap_count++];
    memset(t, 0, sizeof(*t));
    t->type = TRAP_PIT;
    t->x = CX + 9;
    t->y = CY + 9;
    t->respawn_timer = 1;
    static const VoxelType k_crystals[] = {
        VOXEL_CRYSTAL_BLUE,   VOXEL_CRYSTAL_PURPLE, VOXEL_CRYSTAL_RED, VOXEL_CRYSTAL_GREEN,
        VOXEL_CRYSTAL_YELLOW, VOXEL_CRYSTAL_ORANGE, VOXEL_CRYSTAL_CYAN};
    int n = (int)(sizeof(k_crystals) / sizeof(k_crystals[0]));
    for (int i = 0; i < n; i++) {
        fl->crystal_respawns[i] = (CrystalRespawn){CX - 10 + i, CY - 10, k_crystals[i], 1};
    }
    fl->crystal_respawns[n] = (CrystalRespawn){CX, CY - 12, VOXEL_CRYSTAL_RED, 3};
    fl->crystal_respawn_count = n + 1;

    NPC *wounded = monster(10, NULL, FLOOR, CX + 30, CY);
    wounded->respawn_timer = 0;
    wounded->hp = 3;
    static const char *const k_hurt[] = {"Poisoned", "Bleeding", "Burning"};
    for (int i = 0; i < 3; i++) {
        wounded->effects[wounded->effect_count++] =
            (ActiveEffect){k_hurt[i], EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
    }
    NPC *stiff = monster(11, NULL, FLOOR, CX + 30, CY + 10);
    stiff->respawn_timer = 0;
    stiff->effects[stiff->effect_count++] =
        (ActiveEffect){"Paralyzed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 9, false};
    NPC *back = monster(12, NULL, FLOOR, CX + 20, CY + 20);
    npc_set_active(&g_game, back, false);
    fl->entity_grid[CY + 20][CX + 20] = 0;
    back->hp = 0;
    back->respawn_timer = 1;
    NPC *blocked = monster(13, NULL, FLOOR, CX + 21, CY + 20);
    npc_set_active(&g_game, blocked, false);
    blocked->respawn_timer = 1;
    blocked->spawn_x = stiff->x;
    blocked->spawn_y = stiff->y;
    floor_index_rebuild(&g_game);

    rules_force_d20(20);
    round_at(201);
    rules_force_d20(0);
    CHECK(t->active, "the trap did not re-arm");
    CHECK(fl->crystal_respawn_count == 1 && fl->crystal_respawns[0].respawn_timer == 2,
          "%d crystals waiting", fl->crystal_respawn_count);
    for (int i = 0; i < n; i++) {
        CHECK(fl->map.data[0][CY - 10][CX - 10 + i] == k_crystals[i], "crystal %d is not back", i);
    }
    CHECK(!wounded->active, "the wounded monster lives on %d HP", wounded->hp);
    CHECK(!has(stiff->effects, stiff->effect_count, "Paralyzed"), "the paralysis stays");
    CHECK(back->active && back->hp == back->max_hp && back->x == CX + 20, "no respawn");
    CHECK(!blocked->active && blocked->respawn_timer == 5, "a respawn on a taken tile");

    NPC *afraid = monster(14, NULL, FLOOR, CX - 30, CY);
    afraid->respawn_timer = 0;
    afraid->hp = 3;
    rules_force_d20(1);
    round_at(203);
    rules_force_d20(0);
    CHECK(afraid->morale == 1 && has(afraid->effects, afraid->effect_count, "Frightened"),
          "no fear");

    /* Floor 2, nobody alive: one comes back, the ghost first in the table
     * stays where it is. */
    NPC *ghost = monster(0, NULL, 2, 10, 10);
    ghost->is_ghost = true;
    for (int i = 0; i < 5; i++) {
        monster(1 + i, NULL, 2, 20 + 2 * i, 10);
    }
    for (int i = 0; i < 6; i++) {
        NPC *dead = &g_game.npcs[i];
        npc_set_active(&g_game, dead, false);
        g_game.world->floors[2].entity_grid[dead->y][dead->x] = 0;
        dead->respawn_timer = 100000;
    }
    floor_index_rebuild(&g_game);
    round_at(DENSITY_CHECK * 7);
    int alive = 0;
    for (int i = 1; i < 6; i++) {
        alive += g_game.npcs[i].active;
    }
    CHECK(!ghost->active, "a ghost came back from the dead");
    CHECK(alive == 1, "%d monsters came back at once", alive);
    return 0;
}

/* The monsters' minds: a melee monster walks up and strikes; a mage casts
 * missiles while it has slots, and they can kill; the boss charms from
 * afar and poisons up close. */
int test_monster_minds(void) {
    hx_reset_world();
    Client *c = hero(0, "prey", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    g_game.event.type = 0;
    c->max_hp = 100000;

    NPC *brute = monster(10, NULL, FLOOR, CX + 4, CY);
    brute->respawn_timer = 0;
    ai_attach_behavior(brute);
    rules_force_d20(20);
    for (int r = 0; r < 200 && c->hp == 60; r++) {
        round_at(301 + r);
    }
    heard(c);
    rules_force_d20(0);
    CHECK(c->hp < 60 && said("1d20"), "the monster never struck");
    CHECK(abs(brute->x - CX) + abs(brute->y - CY) == 1, "it did not walk up");
    npc_set_active(&g_game, brute, false);
    g_game.world->floors[FLOOR].entity_grid[brute->y][brute->x] = 0;
    brute->respawn_timer = -1;

    NPC *mage = monster(11, NULL, FLOOR, CX + 3, CY);
    mage->respawn_timer = 0;
    mage->archetype = ARCH_CASTER;
    ai_attach_behavior(mage);
    mage->spell_slots[1] = 1;
    c->hp = 60;
    hush(c);
    for (int r = 0; r < 10 && mage->spell_slots[1] > 0; r++) {
        round_at(401 + r);
    }
    heard(c);
    CHECK(mage->spell_slots[1] == 0 && c->hp < 60 && said("Magic Missile"), "no missile");
    mage->spell_slots[1] = 1;
    c->hp = 1;
    hush(c);
    for (int r = 0; r < 10 && mage->spell_slots[1] > 0; r++) {
        round_at(421 + r);
    }
    heard(c);
    CHECK(c->floor_id == 0 && said("You died!"), "the missiles do not kill");
    npc_set_active(&g_game, mage, false);
    mage->respawn_timer = -1;

    /* The boss of floor 10: from three tiles a charm, adjacent a poison. */
    place(c, 10, CX, CY);
    c->max_hp = c->hp = 100000;
    NPC *boss = monster(12, NULL, 10, CX + 3, CY);
    boss->respawn_timer = 0;
    boss->archetype = ARCH_BOSS;
    ai_attach_behavior(boss);
    srand(7);
    rules_force_d20(1);
    for (int r = 0; r < 200 && !has(c->effects, c->effect_count, "Charmed"); r++) {
        boss->x = CX + 3;
        floor_index_rebuild(&g_game);
        round_at(501 + r);
    }
    CHECK(has(c->effects, c->effect_count, "Charmed"), "no charm");
    c->effect_count = 0;
    g_game.world->floors[10].entity_grid[CY][boss->x] = 0;
    boss->x = CX + 1;
    g_game.world->floors[10].entity_grid[CY][CX + 1] = boss->entity_id;
    for (int r = 0; r < 200 && !has(c->effects, c->effect_count, "Poisoned"); r++) {
        round_at(801 + r);
    }
    rules_force_d20(0);
    CHECK(has(c->effects, c->effect_count, "Poisoned"), "no poison");
    return 0;
}

/* The DM's lookups and reports: monsters and items by name, levels,
 * bosses (alive and dead) and wandering merchants floor by floor. */
int test_dm_reports(void) {
    hx_reset_world();
    Client *c = hero(0, "keeper", 1, CX, CY);
    CHECK(c != NULL, "no player");
    c->is_dm = true;

    run(c, "dm_find_monster goblin");
    CHECK(said("ID: ") && said("Goblin"), "no goblin");
    run(c, "dm_find_monster zzzz");
    CHECK(said("No monsters found"), "found zzzz");
    run(c, "dm_find_monster a");
    CHECK(said("only the first 10"), "no cut at 10 monsters");
    run(c, "dm_find_item sword");
    CHECK(said("ID: ") && said("word"), "no sword");
    run(c, "dm_find_item a");
    CHECK(said("only the first 10"), "no cut at 10 items");
    run(c, "dm_find_item zzzz");
    CHECK(said("No items found"), "found zzzz");

    run(c, "dm_level 3");
    CHECK(said("must be between 6 and 20"), "a level down");
    run(c, "dm_level 8");
    CHECK(c->level == 8 && said("Level increased"), "no level up: %d", c->level);
    c->level = 20;
    run(c, "dm_level 20");
    CHECK(said("already at maximum level"), "past 20");
    c->level = 5;

    run(c, "dm_boss 7");
    CHECK(said("first boss floor is 10"), "boss floor 7");
    run(c, "dm_boss 15");
    CHECK(said("Floor 10 or Floor 20"), "boss floor 15");
    run(c, "dm_boss 0");
    CHECK(said("floors 1 to"), "boss floor 0");
    run(c, "dm_boss 10");
    CHECK(said("No bosses found on Floor 10"), "a boss from nowhere");
    run(c, "dm_spawn_boss 0");
    CHECK(said("Use: dm_spawn_boss"), "a boss on floor 0");
    run(c, "dm_spawn_boss 10");
    CHECK(said("successfully spawned on floor 10"), "no boss");
    NPC *boss = NULL;
    for (int i = 0; i < MAX_NPCS && !boss; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].archetype == ARCH_BOSS) {
            boss = &g_game.npcs[i];
        }
    }
    CHECK(boss != NULL, "the boss is not in the table");
    ItemInstance *loot = npc_loot_new(&g_game.pools, g_game.npcs, MAX_NPCS, boss);
    CHECK(loot != NULL, "no loot for the boss");
    int dagger = item_named("Dagger");
    loot[0].template_idx = dagger;
    loot[0].stack_count = 1;
    loot[0].blessing = BLESS_NORMAL;
    loot[0].quality = QUALITY_NORMAL;
    loot[0].is_identified = true;
    boss->spell_slots_max[2] = 3;
    boss->spell_slots[2] = 1;
    boss->effects[boss->effect_count++] =
        (ActiveEffect){"Poisoned", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 4, false};
    run(c, "dm_boss 10");
    CHECK(said("BOSS #1") && said("[DM] AC:") && said("L2:1/3") && said("Dagger") &&
              said("Active Effects (1)") && said("Total active bosses on Floor 10: 1"),
          "the boss report misses something");
    loot[0].template_idx = -1;
    run(c, "dm_boss 10");
    CHECK(said("loot generated on death"), "the empty loot is not told");
    npc_set_active(&g_game, boss, false);
    boss->respawn_timer = 50;
    run(c, "dm_boss 10");
    CHECK(said("DEAD BOSS") && said("Boss defeated, 1 respawning"), "the dead boss is not told");

    run(c, "dm_merchant 3");
    CHECK(said("first merchant floor is 5"), "merchant floor 3");
    run(c, "dm_merchant 12");
    CHECK(said("Floor 5 or Floor 15"), "merchant floor 12");
    run(c, "dm_merchant 99999");
    CHECK(said("floors 1 to"), "merchant floor 99999");
    run(c, "dm_merchant 5");
    CHECK(said("No active wandering merchants"), "a merchant from nowhere");
    for (int k = 0; k < 2; k++) {
        NPC *m = &g_game.npcs[40 + k];
        memset(m, 0, sizeof(*m));
        m->archetype = ARCH_MERCHANT;
        m->entity_id = 9040 + k;
        m->floor_id = 5;
        m->x = 10 + k;
        m->y = 10;
        m->respawn_timer = -1;
        MerchantData *shop = npc_shop_new(&g_game.pools, g_game.npcs, MAX_NPCS, m);
        CHECK(shop != NULL, "no shop for the wagon");
        snprintf(shop->shop_name, sizeof(shop->shop_name), "Wagon %d", k);
        if (k == 0) {
            add_item_to_shop(shop, dagger, 3);
        }
        npc_set_active(&g_game, m, true);
    }
    floor_index_rebuild(&g_game);
    run(c, "dm_merchant 5");
    CHECK(said("WANDERING MERCHANT #2") && said("Goods for sale (Wagon 0)") &&
              said("Goods for sale: empty") && said("Total merchants found on Floor 5: 2"),
          "the merchant report misses something");
    return 0;
}

/* Every class starts with its gear, ordinary and known. */
int test_starting_gear(void) {
    hx_reset_world();
    Client *c = hero(0, "novice", 0, CX, CY);
    CHECK(c != NULL, "no player");
    for (int k = 0; k < CLASS_COUNT; k++) {
        c->backpack_count = 0;
        c->class_id = (ClassType)k;
        give_starting_gear(c);
        CHECK(c->backpack_count > 0, "class %s starts empty-handed", CLASSES[k].name);
        for (int i = 0; i < c->backpack_count; i++) {
            const ItemInstance *it = &c->backpack[i];
            CHECK(it->is_identified && it->blessing == BLESS_NORMAL && it->stack_count == 1,
                  "class %s: %s", CLASSES[k].name, item_database[it->template_idx].name);
        }
    }
    return 0;
}
