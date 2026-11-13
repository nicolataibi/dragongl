/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Text commands in the in-process fixture (see harness_commands.h).
 */

#include "harness_commands.h"

#include "fixture.h"

#include "items.h"
#include "map.h"
#include "server_commands.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "server_tiles.h"
#include "server_world.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

enum { FLOOR = 1, CX = 50, CY = 50, MERCHANT_SLOT = 5 };

static char g_out[1 << 16];
static size_t g_out_len;

/*Byte-wise: the output is a stream of binary messages.*/
static bool said(const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= g_out_len; i++) {
        if (memcmp(g_out + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

static void run(Client *c, const char *line) {
    fx_drain(0);
    handle_text_cmd(c, line, &g_game);
    g_out_len = fx_output(0, g_out, sizeof(g_out));
}

/*A level-5 character alone on an open floor, with an empty backpack.*/
static Client *adventurer(void) {
    static bool made;
    Client *c = &g_test_clients[0];
    if (!made) {
        c = fx_player(0, "shopper");
        if (!c) {
            return NULL;
        }
        made = true;
    }
    memset(g_game.npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int i = 0; i < MAX_NPCS; i++) {
        g_game.npcs[i].respawn_timer = -1;
    }
    Floor *fl = &g_game.world->floors[FLOOR];
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            bool edge = x == 0 || y == 0 || x == MAP_WIDTH - 1 || y == MAP_HEIGHT - 1;
            fl->map.data[0][y][x] = edge ? VOXEL_ROCK : VOXEL_FLOOR;
            fl->entity_grid[y][x] = 0;
        }
    }
    c->is_dm = false;
    c->level = 5;
    c->str = c->dex = c->con = c->intel = c->wis = c->cha = 14;
    c->hp = c->max_hp = 60;
    c->floor_id = FLOOR;
    c->x = CX;
    c->y = CY;
    c->gold = 100000;
    c->effect_count = 0;
    c->backpack_count = 0;
    fx_reset_item(&c->slot_body);
    fx_reset_item(&c->slot_hand_r);
    fx_reset_item(&c->slot_hand_l);
    fl->entity_grid[CY][CX] = c->entity_id;
    floor_index_rebuild(&g_game);
    fx_drain(0);
    return c;
}

/*The first item of a category that costs at least min_cost; -1 if none.*/
static int item_of(ItemCategory category, int min_cost) {
    for (int i = 0; i < item_database_size; i++) {
        if (item_database[i].category == category && item_database[i].cost >= (uint64_t)min_cost) {
            return i;
        }
    }
    return -1;
}

static int item_named(const char *part) {
    for (int i = 0; i < item_database_size; i++) {
        if (strcasestr(item_database[i].name, part)) {
            return i;
        }
    }
    return -1;
}

static NPC *merchant_at(int x, int y, int template_idx, int stock) {
    NPC *m = &g_game.npcs[MERCHANT_SLOT];
    memset(m, 0, sizeof(*m));
    m->archetype = ARCH_MERCHANT;
    m->entity_id = 7000;
    m->floor_id = FLOOR;
    m->x = m->spawn_x = x;
    m->y = m->spawn_y = y;
    m->hp = m->max_hp = 100;
    m->respawn_timer = -1;
    snprintf(m->merchant.shop_name, sizeof(m->merchant.shop_name), "Test Shop");
    add_item_to_shop(m, template_idx, stock);
    npc_set_active(&g_game, m, true);
    g_game.world->floors[FLOOR].entity_grid[y][x] = m->entity_id;
    return m;
}

static ItemInstance *carry(Client *c, int template_idx) {
    ItemInstance *it = &c->backpack[c->backpack_count++];
    fx_reset_item(it);
    it->template_idx = template_idx;
    it->stack_count = 1;
    it->durability = 100;
    it->is_identified = true;
    it->blessing = BLESS_NORMAL;
    return it;
}

/* --- Shops ------------------------------------------------------------------ */

/* Whatever the dice of the haggling say, buying an item and selling it
 * back never leaves the player richer. */
int test_shop_no_arbitrage(void) {
    Client *c = adventurer();
    int sword = item_of(ITEM_WEAPON, 50);
    CHECK(c && sword >= 0, "no player or no weapon worth 50 gp");
    merchant_at(CX + 1, CY, sword, 1000);
    uint64_t start = c->gold;
    srand(11);
    for (int round = 0; round < 300; round++) {
        if (round % 3 == 0) {
            run(c, "haggle buy 1");
        }
        run(c, "buy 1");
        CHECK(c->gold <= start, "round %d: %lu gp after a purchase, started with %lu", round,
              (unsigned long)c->gold, (unsigned long)start);
        if (round % 2 == 0) {
            run(c, "haggle sell 1");
        }
        run(c, "sell 1");
        CHECK(c->gold <= start, "round %d: %lu gp after a sale, started with %lu", round,
              (unsigned long)c->gold, (unsigned long)start);
        CHECK(c->backpack_count <= 1, "round %d: %d items kept", round, c->backpack_count);
    }

    /* A haggled price belongs to the item it was haggled for. */
    int cheap = item_of(ITEM_WEAPON, 1);
    for (int i = 0; i < item_database_size && cheap == sword; i++) {
        if (item_database[i].category == ITEM_WEAPON && item_database[i].cost > 0 && i != sword) {
            cheap = i;
        }
    }
    CHECK(cheap >= 0 && cheap != sword, "no second weapon");
    c->backpack_count = 0;
    carry(c, sword);
    run(c, "haggle sell 1");
    c->backpack_count = 0;
    carry(c, cheap);
    run(c, "sell 1");
    CHECK(!said("negotiated"), "the price haggled for one item was paid for another");
    return 0;
}

int test_shop_needs_a_merchant(void) {
    Client *c = adventurer();
    int sword = item_of(ITEM_WEAPON, 50);
    CHECK(c && sword >= 0, "no player or no weapon");
    carry(c, sword);
    uint64_t gold = c->gold;
    run(c, "sell 1");
    CHECK(said("No merchant nearby") && c->gold == gold && c->backpack_count == 1,
          "sold an item with no merchant around");
    merchant_at(CX + 30, CY + 30, sword, 5);
    run(c, "sell 1");
    CHECK(c->gold == gold && c->backpack_count == 1, "sold to a merchant 30 tiles away");
    run(c, "buy 1");
    CHECK(c->gold == gold && c->backpack_count == 1, "bought from a merchant 30 tiles away");
    return 0;
}

/* A merchant gets its stock back (it never did: its timer was mistaken for
 * the one of a dead monster). */
int test_merchant_restocks(void) {
    Client *c = adventurer();
    int sword = item_of(ITEM_WEAPON, 50);
    CHECK(c && sword >= 0, "no player or no weapon");
    NPC *m = merchant_at(CX + 1, CY, sword, 5);
    m->merchant.item_stock[0] = 0;
    m->merchant.restock_timer = 2;
    run(c, "buy 1");
    CHECK(said("sold out") && c->backpack_count == 0, "bought an item that was sold out");
    for (int tick = 0; tick < 8 && m->merchant.item_stock[0] == 0; tick++) {
        update_world(g_game.clients, &g_game);
        usleep(210 * 1000); /* one round */
    }
    CHECK(m->merchant.item_stock[0] == 5, "stock %d after the restock time",
          m->merchant.item_stock[0]);
    CHECK(m->active, "the merchant vanished");
    return 0;
}

/* --- Equipment ---------------------------------------------------------------- */

int test_wear_cursed_stays(void) {
    Client *c = adventurer();
    int armor = item_of(ITEM_ARMOR, 1);
    CHECK(c && armor >= 0, "no player or no armor");
    fx_reset_item(&c->slot_body);
    c->slot_body.template_idx = armor;
    c->slot_body.stack_count = 1;
    c->slot_body.blessing = BLESS_CURSED;
    carry(c, armor);
    run(c, "wear 1");
    CHECK(said("will not let go"), "no refusal: a cursed armor was replaced");
    CHECK(c->slot_body.blessing == BLESS_CURSED && c->backpack_count == 1 &&
              c->backpack[0].blessing == BLESS_NORMAL,
          "the cursed armor left its slot");
    /* An ordinary one is swapped with the one in the backpack. */
    c->slot_body.blessing = BLESS_NORMAL;
    c->slot_body.durability = 7;
    run(c, "wear 1");
    CHECK(c->slot_body.durability == 100 && c->backpack_count == 1 &&
              c->backpack[0].durability == 7,
          "the worn armor and the carried one were not swapped");
    return 0;
}

/* --- World ---------------------------------------------------------------------- */

/* Floor 20 was solid rock: no room, no stairs, no way further down. */
int test_boss_floor_is_playable(void) {
    static const int floors[] = {10, 20, 30};
    for (size_t k = 0; k < sizeof(floors) / sizeof(floors[0]); k++) {
        if (floors[k] >= MAX_FLOORS) {
            continue;
        }
        Floor *fl = &g_game.world->floors[floors[k]];
        srand(3);
        floor_init(fl, floors[k]);
        int open = 0, up = 0, down = 0;
        for (int y = 0; y < MAP_HEIGHT; y++) {
            for (int x = 0; x < MAP_WIDTH; x++) {
                VoxelType v = fl->map.data[0][y][x];
                open += tile_walkable(fl, x, y);
                up += v == VOXEL_STAIRS_UP;
                down += v == VOXEL_STAIRS_DOWN;
            }
        }
        CHECK(open > 200, "floor %d: %d walkable tiles", floors[k], open);
        CHECK(up > 0, "floor %d: no stairs up", floors[k]);
        CHECK(down > 0 || floors[k] == MAX_FLOORS - 1, "floor %d: no stairs down", floors[k]);
    }
    return 0;
}

int test_tunnel_keeps_the_border(void) {
    Client *c = adventurer();
    int pick = item_named("Pick");
    CHECK(c && pick >= 0, "no player or no pick");
    carry(c, pick);
    Floor *fl = &g_game.world->floors[FLOOR];
    fl->entity_grid[c->y][c->x] = 0;
    c->x = 1;
    fl->entity_grid[c->y][c->x] = c->entity_id;
    run(c, "tunnel w");
    CHECK(said("Beyond borders"), "no refusal at the edge of the map");
    CHECK(fl->map.data[0][c->y][0] == VOXEL_ROCK, "the edge of the map was dug");
    fl->map.data[0][c->y][2] = VOXEL_ROCK;
    run(c, "tunnel e");
    CHECK(!said("Beyond borders"), "an inner tile was taken for the edge");
    return 0;
}

/* --- DM commands -------------------------------------------------------------------- */

static int active_npcs_at(int x, int y) {
    int count = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &g_game.npcs[i];
        count += n->active && n->floor_id == FLOOR && n->x == x && n->y == y;
    }
    return count;
}

int test_dm_spawn_and_place(void) {
    Client *c = adventurer();
    CHECK(c != NULL, "no player");
    c->is_dm = true;
    Floor *fl = &g_game.world->floors[FLOOR];
    char line[64];

    /* A slot whose previous tenant left loot, a name and a timer behind. */
    NPC *stale = &g_game.npcs[0];
    stale->template = &bestiary_data[0];
    stale->respawn_timer = -1;
    stale->gold_drop = 500;
    stale->is_ghost = true;
    stale->ghost_loot[0].template_idx = 1;
    stale->ghost_loot[0].stack_count = 1;
    snprintf(stale->custom_name, sizeof(stale->custom_name), "Ghost of nobody");

    fl->map.data[0][CY][CX + 3] = VOXEL_WALL;
    snprintf(line, sizeof(line), "dm_spawn 0 %d %d", CX + 3, CY);
    run(c, line);
    CHECK(said("not a free") && active_npcs_at(CX + 3, CY) == 0, "spawned inside a wall");
    snprintf(line, sizeof(line), "dm_spawn 0 %d %d", CX, CY);
    run(c, line);
    CHECK(said("not a free") && fl->entity_grid[CY][CX] == c->entity_id,
          "spawned on top of the DM");
    run(c, "dm_spawn 0 -3 999999");
    CHECK(said("not a free"), "coordinates outside the map accepted");
    snprintf(line, sizeof(line), "dm_spawn %d %d %d", bestiary_size, CX + 2, CY);
    run(c, line);
    CHECK(active_npcs_at(CX + 2, CY) == 0, "a monster that is not in the bestiary");

    snprintf(line, sizeof(line), "dm_spawn 0 %d %d", CX + 2, CY);
    run(c, line);
    CHECK(said("Spawned") && active_npcs_at(CX + 2, CY) == 1, "no monster spawned: %s", g_out);
    NPC *n = npc_by_entity_id(&g_game, fl->entity_grid[CY][CX + 2]);
    CHECK(n && n->template == &bestiary_data[0] && n->hp > 0, "the monster is not on the grid");
    CHECK(n->gold_drop == 0 && !n->is_ghost && n->custom_name[0] == '\0' &&
              n->ghost_loot[0].stack_count == 0,
          "the monster inherited the previous tenant of its slot");
    run(c, line);
    CHECK(said("not a free") && active_npcs_at(CX + 2, CY) == 1, "two monsters on one tile");

    /* dm_place puts that very item on the ground. */
    int dagger = item_named("Dagger");
    CHECK(dagger >= 0, "no dagger in the data");
    snprintf(line, sizeof(line), "dm_place %d %d %d", dagger, CX + 1, CY);
    run(c, line);
    CHECK(said("Placed") && active_npcs_at(CX + 1, CY) == 1, "nothing placed: %s", g_out);
    snprintf(line, sizeof(line), "dm_place %d %d %d", item_database_size, CX + 1, CY + 1);
    run(c, line);
    CHECK(active_npcs_at(CX + 1, CY + 1) == 0, "an item that is not in the data");
    fl->entity_grid[c->y][c->x] = 0;
    c->x = CX + 1;
    run(c, "get");
    CHECK(c->backpack_count == 1 && c->backpack[0].template_idx == dagger &&
              c->backpack[0].stack_count == 1,
          "picked up %d item(s), first template %d", c->backpack_count,
          c->backpack_count ? c->backpack[0].template_idx : -1);
    CHECK(active_npcs_at(CX + 1, CY) == 0, "the item is still on the ground");
    return 0;
}

/* Every command that reads a number, with the numbers nobody types. */
int test_numeric_arguments(void) {
    static const char *const numbers[] = {
        "0",          "-1",         "-2147483648", "2147483647",
        "2147483648", "4294967296", "-2147483649", "99999999999999999999999",
        "1e9",        "0x10",       " 7",          "+3",
        "",
    };
    static const char *const commands[] = {
        "browse %s",
        "buy %s",
        "sell %s",
        "haggle buy %s",
        "haggle sell %s",
        "wear %s",
        "lore %s",
        "drop %s",
        "use %s",
        "belt %s",
        "unbelt %s",
        "fill %s",
        "get %s",
        "throw %s e",
        "cast %s %s",
        "identify %s",
        "offer %s %s",
        "dm_gold %s",
        "dm_level %s",
        "dm_spawn %s %s %s",
        "dm_place %s %s %s",
        "dm_goto %s %s %s",
        "dm_shop %s %s",
        "dm_boss %s",
        "dm_merchant %s",
        "dm_spawn_boss %s",
        "dm_pdf %s",
    };
    Client *c = adventurer();
    int sword = item_of(ITEM_WEAPON, 50);
    CHECK(c && sword >= 0, "no player or no weapon");
    merchant_at(CX + 1, CY, sword, 5);
    char line[256];
    for (size_t k = 0; k < sizeof(commands) / sizeof(commands[0]); k++) {
        for (size_t n = 0; n < sizeof(numbers) / sizeof(numbers[0]); n++) {
            const char *v = numbers[n];
            /* Back to a known state: a DM on the open floor with one item. */
            Floor *fl = &g_game.world->floors[c->floor_id];
            if (fl->entity_grid[c->y][c->x] == c->entity_id) {
                fl->entity_grid[c->y][c->x] = 0;
            }
            c->floor_id = FLOOR;
            c->x = CX;
            c->y = CY;
            g_game.world->floors[FLOOR].entity_grid[CY][CX] = c->entity_id;
            c->is_dm = strncmp(commands[k], "dm_", 3) == 0;
            c->level = 5;
            c->backpack_count = 0;
            carry(c, sword);
            snprintf(line, sizeof(line), commands[k], v, v, v);
            run(c, line);
            CHECK(c->floor_id >= 0 && c->floor_id < MAX_FLOORS && c->x >= 0 && c->x < MAP_WIDTH &&
                      c->y >= 0 && c->y < MAP_HEIGHT,
                  "'%s' moved the player to floor %d, %d,%d", line, c->floor_id, c->x, c->y);
            CHECK(c->backpack_count >= 0 && c->backpack_count <= MAX_BACKPACK,
                  "'%s': %d items in the backpack", line, c->backpack_count);
            CHECK(c->level >= 1 && c->level <= 20, "'%s': level %d", line, c->level);
        }
    }
    /* The floor of the DM commands is checked before it indexes anything. */
    c->is_dm = true;
    run(c, "dm_merchant 2147483647");
    CHECK(said("floors 1 to"), "dm_merchant: no refusal of floor 2147483647");
    run(c, "dm_boss -10");
    CHECK(said("floors 1 to"), "dm_boss: no refusal of floor -10");
    run(c, "dm_spawn_boss 100000");
    CHECK(said("floor 1-"), "dm_spawn_boss: no refusal of floor 100000");
    return 0;
}
