/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Characters against monsters and characters with items (see
 * harness_interact.h).
 */

#include "harness_interact.h"

#include "harness_util.h"

#include "ai.h"
#include "inventory.h"
#include "items.h"
#include "player.h"
#include "server_combat.h"
#include "server_internal.h"
#include "server_loot.h"
#include "server_move.h"
#include "server_world.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static void afflict(ActiveEffect *effects, int *count, const char *name, int value) {
    effects[(*count)++] = (ActiveEffect){name, EVENT_ON_TURN_START, MOD_ADDITIVE, value, 5, false};
}

/* The hits of n attacks of the monster on the character, the dice free. */
static int monster_hits(NPC *m, Client *c, int n) {
    int hits = 0;
    for (int k = 0; k < n; k++) {
        c->hp = 1000;
        perform_attack_npc(m, c, &g_game);
        hits += c->hp < 1000;
    }
    c->hp = 60;
    return hits;
}

/* LORE.md, Status Conditions: an invisible attacker has advantage, a
 * frightened or blinded one disadvantage; an invisible target imposes it;
 * a hit on a paralyzed target is a critical, for the monsters too; a
 * petrified monster has +10 AC. */
int test_interact_conditions(void) {
    hx_reset_world();
    Client *c = hero(0, "conditioned", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 10000;

    afflict(c->effects, &c->effect_count, "Invisible", 0);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("[ADVANTAGE]") && c->effect_count == 0, "an invisible attacker: no advantage");
    afflict(c->effects, &c->effect_count, "Frightened", 0);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("[DISADVANTAGE]"), "a frightened attacker: no disadvantage");
    c->effect_count = 0;
    afflict(m->effects, &m->effect_count, "Petrified", 0);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("VS AC 20"), "a petrified monster (AC 10) has no +10");
    m->effect_count = 0;

    /* The monsters: a paralyzed character takes a critical. */
    afflict(c->effects, &c->effect_count, "Paralyzed", 0);
    rules_force_d20(15);
    perform_attack_npc(m, c, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(said("[CRITICAL!]"), "a hit on a paralyzed character is not critical");
    c->effect_count = 0;
    c->hp = 60;

    /* A blinded monster hits less often: about 1 in 4 instead of 1 in 2. */
    m->attack_bonus = 0;
    c->slot_body.template_idx = -1;
    c->dex = 10; /* AC 10: a d20 + 0 hits on 10 or more */
    srand(3);
    int seeing = monster_hits(m, c, 2000);
    afflict(m->effects, &m->effect_count, "Blinded", 0);
    int blind = monster_hits(m, c, 2000);
    m->effect_count = 0;
    CHECK(seeing > 900 && blind < 700, "hits: %d seeing, %d blinded, of 2000", seeing, blind);
    return 0;
}

/* LORE.md, Charmed: the character cannot attack its charmer, with a weapon
 * or a spell, and can attack anything else. */
int test_interact_charm(void) {
    hx_reset_world();
    Client *c = hero(0, "charmed", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *charmer = monster(10, NULL, FLOOR, CX + 1, CY);
    NPC *other = monster(11, NULL, FLOOR, CX - 1, CY);
    charmer->hp = other->hp = 1000;
    afflict(c->effects, &c->effect_count, "Charmed", charmer->entity_id);
    rules_force_d20(20);
    perform_attack(c, charmer, &g_game);
    heard(c);
    CHECK(charmer->hp == 1000 && said("You are charmed"), "the charmer was attacked");
    perform_attack(c, other, &g_game);
    rules_force_d20(0);
    CHECK(other->hp < 1000, "the other monster was spared");
    return 0;
}

/* A monster strikes a character on a diagonal, as a character strikes it. */
int test_interact_reach(void) {
    hx_reset_world();
    Client *c = hero(0, "diagonal", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY + 1);
    rules_force_d20(20);
    hush(c);
    ai_update_npc(&g_game, m, g_test_clients, MAX_CLIENTS, true);
    heard(c);
    rules_force_d20(0);
    CHECK(c->hp < 60 && said("attacks you"), "a diagonal neighbour did not strike");
    CHECK(m->x == CX + 1 && m->y == CY + 1, "the monster moved instead of striking");
    return 0;
}

/* shoot: the weapon's own ammunition from the belt, DEX, its range and the
 * character's sight, the first creature that way or the nearest in line;
 * no ammunition spent when nothing is in range. */
int test_interact_ranged(void) {
    hx_reset_world();
    Client *c = hero(0, "archer2", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int longbow = item_named("Longbow"), arrows = item_named("Arrows (20)");
    int bolts = item_named("Crossbow Bolts"), lantern = item_named("Lantern, Hooded");
    int sling = item_named("Sling"), longsword = item_named("Longsword");
    CHECK(longbow >= 0 && arrows >= 0 && bolts >= 0 && lantern >= 0 && sling >= 0 && longsword >= 0,
          "data");
    CHECK(item_database[longbow].ammo == AMMO_ARROW && item_database[longbow].range == 30 &&
              item_database[bolts].ammo == AMMO_BOLT &&
              item_database[item_named("Heavy Crossbow")].ammo == AMMO_BOLT &&
              item_database[sling].range == 6,
          "longbow: ammo %d range %d", item_database[longbow].ammo, item_database[longbow].range);

    run(c, "shoot");
    CHECK(said("You hold no ranged weapon"), "shot with bare hands");
    c->slot_hand_r = *give(c, longbow, 1);
    c->slot_hand_l = *give(c, lantern, 1);
    c->backpack_count = 0;
    c->str = 8;
    c->dex = 18;
    NPC *m = monster(10, NULL, FLOOR, CX + 5, CY);
    m->hp = m->max_hp = 1000;

    c->belt[0] = *give(c, bolts, 3);
    c->backpack_count = 0;
    run(c, "shoot e");
    CHECK(m->hp == 1000 && said("No arrows on your belt") && c->belt[0].stack_count == 3,
          "bolts shot from a bow");
    c->belt[1] = *give(c, arrows, 2);
    c->backpack_count = 0;
    rules_force_d20(20);
    run(c, "shoot e");
    CHECK(m->hp < 1000 && c->belt[1].stack_count == 1 && said("You shoot your Longbow"),
          "the arrow did not fly");
    /* DEX 18 (+4) and half of level 5 (+2); no disadvantage at range. */
    CHECK(said("] +6 =") && !said("DISADVANTAGE"), "the shot is not DEX +4 and level +2");
    run(c, "shoot n");
    CHECK(said("No creature that way") && c->belt[1].stack_count == 1, "an arrow at nothing");
    run(c, "shoot");
    CHECK(c->belt[1].stack_count == 0 && said("You shoot your Longbow"), "no aim at the nearest");
    c->belt[1] = *give(c, arrows, 5);
    c->backpack_count = 0;
    /* Point-blank: disadvantage. */
    m->x = CX + 1;
    run(c, "shoot e");
    CHECK(said("point-blank") && said("DISADVANTAGE"), "a shot at a neighbour has no disadvantage");
    /* The range of the weapon and the sight. */
    c->slot_hand_r = *give(c, sling, 1);
    c->belt[2] = *give(c, item_named("Sling Bullets"), 5);
    c->backpack_count = 0;
    m->x = CX + 8;
    run(c, "shoot e");
    CHECK(said("within 6 tiles") && c->belt[2].stack_count == 5, "a sling reached 8 tiles");
    c->slot_hand_l.template_idx = -1; /* no light: 4 tiles of sight */
    c->slot_hand_r = *give(c, longbow, 1);
    c->backpack_count = 0;
    m->x = CX + 6;
    run(c, "shoot e");
    rules_force_d20(0);
    CHECK(said("within 4 tiles"), "shot past the sight in the dark");
    return 0;
}

/* The ability of an attack (STR, DEX for ranged weapons, the better of the
 * two for finesse ones) and the quality of what is held and worn (LORE.md:
 * Rusty -1, Fine +1, Masterwork +2). */
int test_interact_ability_and_quality(void) {
    hx_reset_world();
    Client *c = hero(0, "duelist", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 10000;
    c->str = 8;
    c->dex = 18;
    int longsword = item_named("Longsword"), rapier = item_named("Rapier");
    CHECK(longsword >= 0 && rapier >= 0 && item_database[rapier].finesse &&
              !item_database[longsword].finesse,
          "data");
    c->slot_hand_r = *give(c, longsword, 1);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("] +1 ="), "a longsword hits with STR 8 (-1) and level +2");
    c->slot_hand_r = *give(c, rapier, 1);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("] +6 ="), "a rapier hits with DEX 18 (+4) and level +2");
    c->slot_hand_r.quality = QUALITY_RUSTY;
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("] +5 ="), "a rusty rapier: no -1");
    c->slot_hand_r.quality = QUALITY_MASTERWORK;
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("] +8 ="), "a masterwork rapier: no +2");

    int chain = item_named("Chain Mail");
    c->slot_body = *give(c, chain, 1);
    c->backpack_count = 0;
    int ac = get_player_ac(c);
    c->slot_body.quality = QUALITY_RUSTY;
    CHECK(get_player_ac(c) == ac - 1, "rusty armour: AC %d, not %d", get_player_ac(c), ac - 1);
    c->slot_body.quality = QUALITY_FINE;
    CHECK(get_player_ac(c) == ac + 1, "fine armour: AC %d, not %d", get_player_ac(c), ac + 1);
    return 0;
}

/* Bracers count in the abilities, and so do an artifact's own STR, DEX and
 * CON; a worn artifact's to-hit counts in every attack. */
int test_interact_worn_bonuses(void) {
    hx_reset_world();
    Client *c = hero(0, "adorned", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int ts, td, tc, ti, tw, th;
    int bracers = -1;
    for (int i = 0; i < item_database_size && bracers < 0; i++) {
        if (item_database[i].category == ITEM_BRACELET) {
            bracers = i;
        }
    }
    CHECK(bracers >= 0, "no bracers");
    c->slot_arm_r = *give(c, bracers, 1);
    c->backpack_count = 0;
    item_database[bracers].str_bonus += 2; /* whatever the data says, +2 more */
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    item_database[bracers].str_bonus -= 2;
    int base_str = 14 + item_database[bracers].str_bonus;
    CHECK(ts == base_str + 2, "the bracers' STR does not count: %d", ts);
    c->slot_arm_r.template_idx = -1;

    int gloves = item_named("Gloves of Swimming");
    CHECK(gloves >= 0, "no gloves");
    c->slot_hands = *give(c, gloves, 1);
    c->backpack_count = 0;
    c->slot_hands.is_artifact = true;
    c->slot_hands.artifact_dex_bonus = 4;
    c->slot_hands.to_hit_bonus = 3;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(td == 14 + 4, "the artifact's DEX does not count: %d", td);
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 10000;
    perform_attack(c, m, &g_game);
    heard(c);
    /* Bare hands: STR 14 (+2), level +2, the gloves +3. */
    CHECK(said("] +7 ="), "the gloves' to-hit does not count");
    return 0;
}

/* Every artifact drops as an item of its kind: a ring is a ring, a shield
 * a shield (every one was the same armour). */
int test_interact_artifacts(void) {
    hx_reset_world();
    Client *c = hero(0, "collector", 99, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, 99, CX + 1, CY);
    static const struct {
        const char *name;
        ItemCategory cat;
    } k_expect[] = {
        {"The Void Blade", ITEM_WEAPON},      {"The Aegis Shield", ITEM_SHIELD},
        {"The Staff of Aurora", ITEM_WEAPON}, {"The Shadow Tunic", ITEM_ARMOR},
        {"The Bow of Destiny", ITEM_WEAPON},  {"Sage's Ring", ITEM_RING},
        {"Boots of the Wind", ITEM_FEET},     {"Amulet of Life", ITEM_NECK},
        {"Hero's Cloak", ITEM_BACK},          {"Bracers of Strength", ITEM_BRACELET},
    };
    int found = 0;
    for (int k = 0; k < 4000 && found < (int)(sizeof(k_expect) / sizeof(k_expect[0])); k++) {
        c->backpack_count = 0;
        drop_loot_from_monster(&g_game, c, m);
        if (c->backpack_count == 0 || !c->backpack[0].is_artifact) {
            continue;
        }
        const ItemInstance *a = &c->backpack[0];
        for (size_t e = 0; e < sizeof(k_expect) / sizeof(k_expect[0]); e++) {
            if (strcmp(a->artifact_name, k_expect[e].name) == 0) {
                CHECK(item_database[a->template_idx].category == k_expect[e].cat, "%s is a %s",
                      a->artifact_name, item_database[a->template_idx].name);
                found++;
            }
        }
    }
    CHECK(found == (int)(sizeof(k_expect) / sizeof(k_expect[0])), "%d artifacts seen", found);
    return 0;
}

/* A drop that finds the backpack full lies on the ground where the monster
 * fell (it was lost). */
int test_interact_loot_full_backpack(void) {
    hx_reset_world();
    Client *c = hero(0, "hoarder", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int rock = item_named("Longsword");
    while (c->backpack_count < MAX_BACKPACK) {
        give(c, rock, 1);
    }
    NPC *m = monster(10, NULL, FLOOR, CX + 3, CY);
    m->archetype = ARCH_ASSASSIN; /* drops weapons: never a stack */
    bool on_ground = false;
    for (int k = 0; k < 200 && !on_ground; k++) {
        drop_loot_from_monster(&g_game, c, m);
        for (int i = 0; i < MAX_NPCS && !on_ground; i++) {
            const NPC *n = &g_game.npcs[i];
            on_ground = n->active && n->archetype == ARCH_TREASURE && n->x == CX + 3 && n->y == CY;
        }
    }
    CHECK(on_ground && c->backpack_count == MAX_BACKPACK, "the drop of a full backpack is lost");
    return 0;
}

/* A step into one's own ghost lays it to rest with what the tombstone
 * holds; another character's step attacks it (it took it like loot, and
 * the ghost vanished). */
int test_interact_ghosts(void) {
    hx_reset_world();
    remove_with_prefix("state-interact_ghosts/saves", "tombstone_");
    Client *owner = hero(0, "haunter", FLOOR, CX, CY);
    Client *other = hero(1, "passerby", FLOOR, CX + 5, CY);
    CHECK(owner && other, "no players");
    give(owner, item_named("Longsword"), 1);
    owner->gold = 77;
    save_bones(&g_game, owner);
    NPC *ghost = NULL;
    for (int i = 0; i < MAX_NPCS && !ghost; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].is_ghost) {
            ghost = &g_game.npcs[i];
        }
    }
    CHECK(ghost != NULL, "no ghost");
    int hp = ghost->hp;
    place(other, FLOOR, ghost->x - 1, ghost->y);
    MsgMove east = {.dx = 1, .dy = 0};
    other->last_action_ms = 0;
    rules_force_d20(20);
    hush(other);
    player_move(other, &east, g_test_clients, &g_game);
    heard(other);
    rules_force_d20(0);
    CHECK(ghost->active && ghost->hp < hp && said("Attack Roll"),
          "a stranger's step did not attack the ghost");
    place(owner, FLOOR, ghost->x, ghost->y + 1);
    MsgMove north = {.dx = 0, .dy = -1};
    owner->last_action_ms = 0;
    hush(owner);
    player_move(owner, &north, g_test_clients, &g_game);
    heard(owner);
    CHECK(!ghost->active && said("recovered your soul") && owner->gold == 77 &&
              owner->backpack_count == 1,
          "the owner's step did not lay the ghost to rest");
    return 0;
}

/* Only the light that shows the way burns; a stack of torches lights the
 * next one when one is spent (the spares on the belt burned too). */
int test_interact_light(void) {
    hx_reset_world();
    Client *c = hero(0, "torchbearer", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int lantern = item_named("Lantern, Hooded"), torch = item_named("Torch");
    CHECK(lantern >= 0 && torch >= 0, "data");
    c->slot_hand_l = *give(c, lantern, 1);
    c->belt[0] = *give(c, torch, 3);
    c->backpack_count = 0;
    int lantern_fuel = c->slot_hand_l.durability, torch_fuel = c->belt[0].durability;
    for (int t = 1; t <= 5; t++) {
        g_game.total_turns = t;
        update_world(g_game.clients, &g_game);
    }
    CHECK(c->slot_hand_l.durability == lantern_fuel - 5 && c->belt[0].durability == torch_fuel,
          "lantern %d of %d, spare torches %d of %d", c->slot_hand_l.durability, lantern_fuel,
          c->belt[0].durability, torch_fuel);
    c->slot_hand_l.template_idx = -1;
    c->belt[0].durability = 1;
    hush(c);
    g_game.total_turns = 7;
    update_world(g_game.clients, &g_game);
    heard(c);
    CHECK(c->belt[0].stack_count == 2 &&
              c->belt[0].durability == item_database[torch].max_durability &&
              said("you light another"),
          "the next torch was not lit: %d left, fuel %d", c->belt[0].stack_count,
          c->belt[0].durability);
    return 0;
}
