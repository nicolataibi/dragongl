/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Melee combat, loot, death, tombstones, ghosts and the combat log, in the
 * in-process fixture (see harness_combat.h). The d20 is forced where the
 * outcome matters; the other dice are free, so the checks bound them.
 */

#include "harness_combat.h"

#include "harness_util.h"

#include "items.h"
#include "player.h"
#include "server_combat.h"
#include "server_loot.h"
#include "server_entities.h"
#include "server_internal.h"
#include "combat_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

/* A hit, a miss, a critical, a paralyzed attacker, the neighbours that
 * turn on the attacker, a kill and its experience. */
int test_melee_attack(void) {
    hx_reset_world();
    Client *c = hero(0, "fighter", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, "Goblin", FLOOR, CX + 1, CY);
    NPC *near = monster(11, "Goblin", FLOOR, CX + 3, CY + 2);
    NPC *far = monster(12, "Goblin", FLOOR, CX + 20, CY);

    rules_force_d20(1);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(m->hp == 30 && said("Missed"), "a natural 1 hit");
    CHECK(said("with your bare hands"), "the unarmed attack is not announced");
    CHECK(near->ai_ctx.current_target_id == c->entity_id, "a neighbour did not turn");
    CHECK(far->ai_ctx.current_target_id == -1, "a monster 20 steps away turned");

    rules_force_d20(20);
    perform_attack(c, m, &g_game);
    heard(c);
    /* 2d4 + STR +2, at least 1: 4 to 10. */
    CHECK(said("[CRITICAL!]") && said("devastating blow"), "no critical");
    CHECK(30 - m->hp >= 4 && 30 - m->hp <= 10, "critical dealt %d", 30 - m->hp);

    /* A paralyzed character does nothing. */
    int hp = m->hp;
    c->effects[c->effect_count++] =
        (ActiveEffect){"Paralyzed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(m->hp == hp && said("cannot act"), "a paralyzed character attacked");
    c->effect_count = 0;

    /* An attack ends every Invisible effect and keeps the others; the
     * effect moved into a freed slot is checked again. */
    const ActiveEffect invisible = {"Invisible", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    c->effects[c->effect_count++] =
        (ActiveEffect){"Poisoned", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    c->effects[c->effect_count++] = invisible;
    c->effects[c->effect_count++] = invisible;
    client_reveal(c);
    heard(c);
    CHECK(c->effect_count == 1 && strcmp(c->effects[0].name, "Poisoned") == 0 &&
              said("visible again"),
          "reveal left %d effects", c->effect_count);
    c->effect_count = 0;

    /* A hit on a paralyzed target is a critical. */
    m->effects[m->effect_count++] =
        (ActiveEffect){"Paralyzed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    rules_force_d20(15);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("[CRITICAL!]"), "a hit on a paralyzed target is not critical");
    m->effect_count = 0;

    /* The kill: experience, the victory line, the monster gone. */
    m->hp = 1;
    rules_force_d20(20);
    perform_attack(c, m, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(!m->active && m->respawn_timer == RESPAWN_TICKS, "the monster survived");
    CHECK(c->xp == 50 && said("[VICTORY]"), "xp %d after a kill worth 50", c->xp);
    CHECK(g_game.world->floors[FLOOR].entity_grid[CY][CX + 1] == 0, "the corpse holds its tile");
    return 0;
}

/* A weapon's dice and the bow: a shot spends an arrow of the belt and
 * none is fired without one. */
int test_weapons_and_arrows(void) {
    hx_reset_world();
    Client *c = hero(0, "archer", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 1000;
    int bow = item_named("Shortbow");
    int ammo = item_of(ITEM_AMMO);
    CHECK(bow >= 0 && ammo >= 0, "no shortbow or no ammunition in the data");
    c->slot_hand_r = *give(c, bow, 1);
    c->backpack_count = 0;

    rules_force_d20(20);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("No arrows on your belt") && m->hp == 1000, "a shot without arrows");

    c->belt[0].template_idx = ammo;
    c->belt[0].stack_count = 2;
    perform_attack(c, m, &g_game);
    perform_attack(c, m, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(m->hp < 1000 && said("point-blank"), "the bow did not shoot");
    CHECK(c->belt[0].template_idx == -1, "the last arrow is still on the belt");
    return 0;
}

/* Monsters hit back: a hit costs hit points and wears the armour 3 times
 * in 100 (an artifact never), a natural 1 misses, a charmed monster does
 * not attack, and each archetype has its own line. */
int test_monster_attacks(void) {
    hx_reset_world();
    Client *c = hero(0, "target", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int armour = item_of(ITEM_ARMOR);
    CHECK(armour >= 0, "no armour in the data");
    c->slot_body = *give(c, armour, 1);
    c->backpack_count = 0;
    int durability = c->slot_body.durability;
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);

    rules_force_d20(20);
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(c->hp <= 58 && c->hp >= 48, "a critical 2d6 left %d of 60 HP", c->hp);
    CHECK(said("You take") && said("[CRITICAL!]"), "the hit is not told");
    /* LORE.md, Combat Wear: a hit received wears the armour 3% of the time. */
    for (int k = 0; k < 400; k++) {
        c->hp = 60;
        perform_attack_npc(m, c, &g_game);
    }
    int worn = durability - c->slot_body.durability;
    CHECK(worn > 0 && worn < 40, "400 hits wore the armour %d points", worn);
    c->slot_body.is_artifact = true;
    durability = c->slot_body.durability;
    for (int k = 0; k < 400; k++) {
        c->hp = 60;
        perform_attack_npc(m, c, &g_game);
    }
    CHECK(c->slot_body.durability == durability, "an artifact wore out");
    c->slot_body.is_artifact = false;
    c->hp = 60;
    hush(c);

    rules_force_d20(1);
    int hp = c->hp;
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(c->hp == hp && said("missed you"), "a natural 1 hit");

    m->effects[m->effect_count++] =
        (ActiveEffect){"Charmed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
    rules_force_d20(20);
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(c->hp == hp && !said("[DANGER]"), "a charmed monster attacked");
    m->effect_count = 0;

    static const struct {
        EntityArchetype arch;
        const char *line;
    } lines[] = {{ARCH_ASSASSIN, "slips into the shadows"},
                 {ARCH_DRAGON, "breathes fire at you"},
                 {ARCH_BRUTE, "devastating blow"}};
    rules_force_d20(1);
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        m->archetype = lines[i].arch;
        perform_attack_npc(m, c, &g_game);
        heard(c);
        CHECK(said(lines[i].line), "archetype %d: no '%s'", (int)lines[i].arch, lines[i].line);
    }
    rules_force_d20(0);
    return 0;
}

/* Loot: about LOOT_DROP_CHANCE kills in a hundred leave something in the
 * backpack; with a full backpack the loot is lost, and said so. */
int test_loot(void) {
    hx_reset_world();
    Client *c = hero(0, "looter", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    srand(7);
    int drops = 0;
    for (int k = 0; k < 40; k++) {
        NPC *m = monster(10, NULL, 2 + (k % 3) * 20, CX + 1, CY);
        hush(c);
        int before = c->backpack_count;
        drop_loot_from_monster(&g_game, c, m);
        heard(c);
        if (said("[LOOT]")) {
            drops++;
        }
        CHECK(c->backpack_count <= before + 1, "two items from one kill");
        c->backpack_count = 0;
        for (int i = 0; i < MAX_BACKPACK; i++) {
            fx_reset_item(&c->backpack[i]);
        }
    }
    CHECK(drops >= 10 && drops <= 36, "%d drops in 40 kills at %d%%", drops, LOOT_DROP_CHANCE);
    int potion = item_of(ITEM_CONSUMABLE);
    CHECK(potion >= 0, "no potion in the data");
    while (c->backpack_count < MAX_BACKPACK) {
        give(c, potion, 1);
    }
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    bool lost = false;
    for (int k = 0; k < 20 && !lost; k++) {
        hush(c);
        drop_loot_from_monster(&g_game, c, m);
        heard(c);
        lost = said("Backpack full");
    }
    CHECK(lost && c->backpack_count == MAX_BACKPACK, "loot into a full backpack");
    return 0;
}

/* The XP of a kill is shared by the party members on the floor. */
int test_party_shares_xp(void) {
    hx_reset_world();
    Client *a = hero(0, "leader", FLOOR, CX, CY);
    Client *b = hero(1, "member", FLOOR, CX, CY + 1);
    Client *away = hero(2, "away", 2, CX, CY);
    CHECK(a && b && away, "no players");
    strcpy(a->party_leader, "leader");
    strcpy(b->party_leader, "leader");
    strcpy(away->party_leader, "leader");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->xp_reward = 100;
    int gained = npc_killed(&g_game, a, m, "sword");
    heard(b);
    CHECK(gained == 50 && a->xp == 50 && b->xp == 50, "shares %d / %d / %d", gained, a->xp, b->xp);
    CHECK(away->xp == 0, "a member on another floor got experience");
    CHECK(said("[PARTY] You receive 50 XP"), "the member was not told");
    heard(a);
    CHECK(said("falls to your sword"), "the killer was not told how");
    a->party_leader[0] = b->party_leader[0] = away->party_leader[0] = '\0';
    return 0;
}

/* The gold of the piles within 3 tiles of (x, y) on floor f. */
static uint64_t gold_around(int f, int x, int y) {
    uint64_t sum = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &g_game.npcs[i];
        if (n->active && n->archetype == ARCH_GOLD && n->floor_id == f && abs(n->x - x) <= 3 &&
            abs(n->y - y) <= 3) {
            sum += (uint64_t)n->gold_drop;
        }
    }
    return sum;
}

/* A boss is defeated by the party in the fight, not by the whole floor:
 * they get the trophy, and its bounty lies around the kill (LORE.md,
 * Boss Encounter System); killed again by the same players it pays
 * nothing. A skeleton counts for the event. */
int test_boss_and_event_kills(void) {
    hx_reset_world();
    Client *a = hero(0, "slayer", 10, CX, CY);
    Client *b = hero(1, "witness", 10, CX + 5, CY);
    Client *c = hero(2, "elsewhere", FLOOR, CX, CY);
    Client *d = hero(3, "faraway", 10, CX + 40, CY);
    CHECK(a && b && c && d, "no players");
    NPC *boss = monster(10, NULL, 10, CX + 1, CY);
    boss->archetype = ARCH_BOSS;
    snprintf(boss->custom_name, sizeof(boss->custom_name), "Lord of Tests");
    uint64_t gold = a->gold;
    uint64_t lying = gold_around(10, CX + 1, CY);
    npc_killed(&g_game, a, boss, NULL);
    heard(b);
    CHECK(a->gold == gold && b->gold == gold, "the bounty went to the purses: %lu / %lu",
          (unsigned long)a->gold, (unsigned long)b->gold);
    CHECK(gold_around(10, CX + 1, CY) == lying + 10000, "bounty on the ground: %lu",
          (unsigned long)(gold_around(10, CX + 1, CY) - lying));
    CHECK((a->bosses_defeated & 1u) && (b->bosses_defeated & 1u), "boss flag not set");
    CHECK(c->bosses_defeated == 0, "a player on another floor got the trophy");
    CHECK(d->bosses_defeated == 0, "a player far from the fight got the trophy");
    CHECK(said("Boss Lord of Tests has been defeated! Its bounty of 10000 gold"),
          "the party was not told");

    /* A pile of the bounty is picked up like any other gold. */
    NPC *pile = NULL;
    for (int i = 0; i < MAX_NPCS && !pile; i++) {
        NPC *n = &g_game.npcs[i];
        if (n->active && n->archetype == ARCH_GOLD && n->floor_id == 10 &&
            abs(n->x - (CX + 1)) <= 3 && abs(n->y - CY) <= 3) {
            pile = n;
        }
    }
    CHECK(pile != NULL, "no pile of the bounty");
    uint64_t share = (uint64_t)pile->gold_drop;
    place(b, 10, pile->x, pile->y + 1);
    run(b, "get n");
    CHECK(b->gold == gold + share && !pile->active, "the pile of %lu gp was not picked up",
          (unsigned long)share);

    /* The same party kills it again: the stairs, no gold. */
    lying = gold_around(10, CX + 2, CY);
    NPC *again = monster(11, NULL, 10, CX + 2, CY);
    again->archetype = ARCH_BOSS;
    npc_killed(&g_game, a, again, NULL);
    heard(b);
    CHECK(gold_around(10, CX + 2, CY) == lying, "a respawned boss paid the veterans again");
    CHECK(said("defeated again"), "no word on the second kill");

    /* A boss killed by the environment still opens the floor. */
    a->bosses_defeated = b->bosses_defeated = 0;
    NPC *trap = monster(13, NULL, 10, CX + 2, CY);
    trap->archetype = ARCH_BOSS;
    npc_killed(&g_game, NULL, trap, NULL);
    CHECK((a->bosses_defeated & 1u) && (b->bosses_defeated & 1u),
          "a boss killed by a trap gave nothing");

    NPC *skel = monster(12, "Skeleton", FLOOR, CX + 1, CY);
    CHECK(skel->template && strcasestr(skel->template->name, "Skeleton"), "no skeleton");
    g_game.event.type = 1;
    g_game.event.floor_id = FLOOR;
    g_game.event.goal = 5;
    g_game.event.progress = 0;
    npc_killed(&g_game, c, skel, NULL);
    heard(c);
    CHECK(g_game.event.progress == 1 && said("Progress: 1/5"), "the event did not count");
    g_game.event.type = 0;
    return 0;
}

/* Death in the dungeon: the character wakes in town with nothing, a
 * tombstone (memory and disk) and a ghost wait where it fell; only its
 * owner takes the tombstone back, then it disappears. */
int test_death_and_tombstone(void) {
    hx_reset_world();
    remove_with_prefix("state-death_and_tombstone/saves", "tombstone_");
    Client *c = hero(0, "unlucky", FLOOR, CX, CY);
    Client *thief = hero(1, "thief", FLOOR, CX + 1, CY + 1);
    CHECK(c && thief, "no players");
    int sword = item_of(ITEM_WEAPON);
    int potion = item_of(ITEM_CONSUMABLE);
    CHECK(sword >= 0 && potion >= 0, "no weapon or potion in the data");
    c->slot_hand_r = *give(c, sword, 1);
    c->backpack_count = 0;
    give(c, potion, 3);
    c->gold = 777;
    c->hp = 1;
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);

    rules_force_d20(20);
    perform_attack_npc(m, c, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(said("You died!"), "no death");
    CHECK(c->floor_id == 0 && c->hp == c->max_hp, "floor %d, HP %d", c->floor_id, c->hp);
    CHECK(c->gold == 0 && c->backpack_count == 0 && c->slot_hand_r.template_idx == -1,
          "the character kept its things");
    const Tombstone *t = &g_game.tombstones[0];
    CHECK(t->active && t->floor_id == FLOOR && t->x == CX && t->y == CY && t->gold == 777,
          "no tombstone where the character fell");
    CHECK(strcmp(t->owner, "unlucky") == 0 && t->backpack_count == 1 &&
              t->slot_hand_r.template_idx == sword,
          "the tombstone lost something");
    CHECK(file_with_prefix("state-death_and_tombstone/saves", "tombstone_unlucky_"),
          "the tombstone is not on disk");
    int ghost = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].is_ghost &&
            strcmp(g_game.npcs[i].custom_name, "Ghost of unlucky") == 0) {
            ghost = i;
        }
    }
    CHECK(ghost >= 0, "no ghost");

    /* A death in town leaves nothing behind. */
    Client *townie = hero(2, "townie", 0, CX, CY);
    CHECK(townie != NULL, "no player");
    save_bones(&g_game, townie);
    CHECK(!g_game.tombstones[1].active, "a tombstone in town");

    /* The tombstone survives a restart; the list shows what it holds. */
    tombstone_load_all(&g_game);
    CHECK(g_game.tombstones[0].active || g_game.tombstones[1].active, "not reloaded");
    place(thief, FLOOR, CX, CY - 1);
    run(thief, "list");
    CHECK(said("TOMBSTONE - unlucky") && said("[GOLD] 777 gp") && said("--- BACKPACK ---"),
          "the list does not show the tombstone");
    place(thief, FLOOR, CX, CY);
    run(thief, "pickup");
    CHECK(said("belongs to unlucky") && thief->backpack_count == 0, "a thief took the tombstone");
    place(thief, FLOOR, CX + 5, CY);

    /* The tombstone is emptied standing on it, not from the next tile. */
    place(c, FLOOR, CX, CY + 1);
    run(c, "pickup");
    CHECK(said("no tombstone under your feet") && c->gold == 0, "picked up from the next tile");
    place(c, FLOOR, CX, CY);
    run(c, "pickup");
    CHECK(said("recovered your items") && c->gold == 777, "gold %lu after the pickup",
          (unsigned long)c->gold);
    CHECK(c->slot_hand_r.template_idx == sword && c->backpack_count == 1 &&
              c->backpack[0].stack_count == 3,
          "the things did not come back");
    CHECK(!file_with_prefix("state-death_and_tombstone/saves", "tombstone_unlucky_"),
          "the tombstone file outlived the pickup");
    run(c, "pickup");
    CHECK(said("no tombstone under your feet"), "a second pickup");

    /* The ghost gives the soul back once. */
    run(c, "retrieve");
    CHECK(said("recovered your soul") && !g_game.npcs[ghost].active, "no retrieve");
    run(c, "retrieve");
    CHECK(said("No ghost of yours next to you"), "a second retrieve");
    return 0;
}

/* A ghost fights as a boss but is not one: dying on a boss floor and
 * killing one's own ghost opens nothing and pays nothing. */
int test_ghost_is_not_a_boss(void) {
    hx_reset_world();
    remove_with_prefix("state-ghost_is_not_a_boss/saves", "tombstone_");
    Client *c = hero(0, "haunted", 10, CX, CY);
    CHECK(c != NULL, "no player");
    save_bones(&g_game, c);
    NPC *ghost = NULL;
    for (int i = 0; i < MAX_NPCS && !ghost; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].is_ghost) {
            ghost = &g_game.npcs[i];
        }
    }
    CHECK(ghost != NULL && ghost->archetype == ARCH_BOSS, "no ghost");
    CHECK(ghost->gold_drop == 0 && ghost->xp_reward == 0, "the ghost pays %d gold, %d XP",
          ghost->gold_drop, ghost->xp_reward);
    c->gold = 0;
    int xp = npc_killed(&g_game, c, ghost, NULL);
    CHECK(xp == 0 && c->gold == 0 && c->bosses_defeated == 0,
          "killing one's ghost paid %d XP, %lu gold, flags %u", xp, (unsigned long)c->gold,
          c->bosses_defeated);
    return 0;
}

/* A tombstone older than its expiry is deleted at load, with its file. */
int test_tombstone_expires(void) {
    hx_reset_world();
    remove_with_prefix("state-tombstone_expires/saves", "tombstone_");
    Client *c = hero(0, "ancient", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->gold = 5;
    save_bones(&g_game, c);
    Tombstone *t = &g_game.tombstones[0];
    CHECK(t->active, "no tombstone");
    /* The file is named after the time of death: the old name goes. */
    char path[256];
    snprintf(path, sizeof(path), "state-tombstone_expires/saves/tombstone_ancient_%lld_%d_%d.dat",
             (long long)t->death_time, t->x, t->y);
    CHECK(unlink(path) == 0, "no file %s", path);
    t->death_time = time(NULL) - 3600L * 24 * 30;
    tombstone_save(t);
    tombstone_load_all(&g_game);
    CHECK(!g_game.tombstones[0].active, "a month-old tombstone was loaded");
    CHECK(!file_with_prefix("state-tombstone_expires/saves", "tombstone_ancient_"),
          "the expired file is still there");
    return 0;
}

/* The combat log: one JSON object per attack and death, names escaped. */
int test_combat_log(void) {
    hx_reset_world();
    clog_init("state-combat_log/combat_log.json");
    Client *c = hero(0, "logger", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    snprintf(m->custom_name, sizeof(m->custom_name), "Bob \"the\" Tab\t");
    m->hp = 1;
    rules_force_d20(20);
    perform_attack(c, m, &g_game);
    rules_force_d20(0);
    clog_spell("logger", "Fire Bolt", "dummy", 7, false);
    clog_save("logger", "Poisoned", 12, 2, 13, true);
    clog_close();
    FILE *f = fopen("state-combat_log/combat_log.json", "r");
    CHECK(f != NULL, "no log file");
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    CHECK(buf[0] == '[' && strstr(buf, "\n]\n"), "not a JSON array: %s", buf);
    CHECK(strstr(buf, "\"type\":\"attack\"") && strstr(buf, "\"crit\":true"), "no attack: %s", buf);
    CHECK(strstr(buf, "\"type\":\"death\"") && strstr(buf, "\"killer\":\"logger\""), "no death: %s",
          buf);
    CHECK(strstr(buf, "\"type\":\"spell\"") && strstr(buf, "\"type\":\"save\""),
          "no spell or save: %s", buf);
    CHECK(strstr(buf, "Bob \\\"the\\\" Tab\\t"), "the name is not escaped: %s", buf);
    return 0;
}
