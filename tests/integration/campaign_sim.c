/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 *
 * The campaign: can the game be played to its end? (tools/quality.sh
 * campaign)
 *
 * A whole world is generated and a character of every class is created
 * as a player creates one (the starting gear, slots and spells of its
 * class). Each one is played by a bot, in the same process as the server
 * and with the same code: every action is a command or a step the server
 * judges (handle_text_cmd, player_move), the world advances one round at
 * a time (update_world). The bot knows the map and where the monsters are
 * (as a player who has learned the dungeon), and plays as a careful one:
 *   - in town it rests, spends its stat points, studies its books in the
 *     temple of its class after a level up, sells its loot (what no
 *     merchant takes stays on the ground), buys healing potions, rations,
 *     a light, arrows for a ranger, the best armour, shield and weapon it
 *     can afford and the next books of its class, then goes down (the
 *     transit spell of its class to the deepest floor it has seen, else
 *     the stairs);
 *   - in the dungeon it eats when hungry, drinks or heals itself when hurt,
 *     goes back to town when it runs out of potions and is badly hurt, when
 *     its backpack is full or a ranger's arrows are spent, fights what is
 *     next to it (a foe its blows do not harm gets a spell, or is left
 *     alone), shoots or casts its best spell at what is near, and hunts the
 *     monsters of the floor while its level is under 1 + floor / 5 (three
 *     more before a boss: too weak, it goes back up the stairs); else it
 *     walks to the stairs down, over the loot, through the boss of a boss
 *     floor; after a death it walks to its ghost and takes its belongings
 *     back.
 * Errands in town cost TOWN_ROUNDS rounds of the world.
 *
 * The report: for every class the deepest floor, the level, the deaths,
 * the rounds (game time: 5 a second) to reach every tenth floor, and why it
 * stopped (the conquest, the rounds allowed, or a character who made no
 * progress for STALL_ROUNDS rounds: a soft-lock or a wall of difficulty).
 *
 *   campaign_sim [rounds] [seed] [json output]
 *
 * Exit status 0 when a character conquered the dungeon, 1 when none did.
 *
 * The server log is dropped (DGL_CAMPAIGN_LOG=1 keeps it on stdout). To
 * follow one bot, DGL_CAMPAIGN_TRACE=<slot> (the class number): its state
 * every 2000 rounds, the boss of a boss floor and the commands the server
 * refused; with DGL_CAMPAIGN_PACK=1 its backpack, DGL_CAMPAIGN_MAP=1 the
 * tiles around it, DGL_CAMPAIGN_NPCS=1 who holds the table of the NPCs.
 */

#include "fixture.h"

#include "ai.h"
#include "inventory.h"
#include "map.h"
#include "player.h"
#include "server_cmd.h"
#include "server_commands.h"
#include "server_conquest.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_move.h"
#include "server_startup.h"
#include "server_tiles.h"
#include "server_world.h"
#include "spells.h"
#include "strnum.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BOTS          CLASS_COUNT
#define TOWN_ROUNDS   300
#define STALL_ROUNDS  60000 /*3 hours and 20 minutes of game*/
#define DEFAULT_LIMIT 2000000

typedef enum { GOAL_STAIRS, GOAL_HUNT, GOAL_BOSS, GOAL_GHOST, GOAL_UP, GOAL_COUNT } Goal;

typedef struct {
    Client *c;
    int deaths;
    int deepest;
    int busy;                    /*rounds of errands still to wait*/
    int progress_round;          /*last round with a new floor or level*/
    int best_level;              /*for the progress*/
    int floor_round[MAX_FLOORS]; /*round of the first arrival, 0: never*/
    int floor_level[MAX_FLOORS]; /*level at the first arrival*/
    int conquered_round;         /*0: not yet*/
    int stalled_round;           /*0: still progressing*/
    char stall_where[96];
    unsigned char blocked[MAP_HEIGHT][MAP_WIDTH]; /*a step there failed*/
    int blocked_floor;
    bool errands_done;              /*in town: errands done, going down when not busy*/
    int last_floor;                 /*after the last round: a death is a fall to town*/
    int miss_round[GOAL_COUNT];     /*a search for the goal found nothing then...*/
    int miss_floor[GOAL_COUNT];     /*...on this floor: no new search for a while*/
    int foe_id, foe_hp, foe_rounds; /*a blow that does no harm (an immune foe)*/
} Bot;

static Bot g_bots[BOTS];
static int g_round;

/* --- what the bot reads of the world --------------------------------------------- */

static bool hostile(const NPC *n, const Client *c) {
    if (!n->active || n->floor_id != c->floor_id) {
        return false;
    }
    if (n->archetype == ARCH_MERCHANT || n->archetype == ARCH_TREASURE ||
        n->archetype == ARCH_GOLD || npc_is_ally(n)) {
        return false;
    }
    return !(n->is_ghost && ghost_is_of(n, c));
}

static NPC *npc_at(int floor, int x, int y) {
    int id = g_game.world->floors[floor].entity_grid[y][x];
    return id > 0 ? npc_by_entity_id(&g_game, id) : NULL;
}

static bool carries_book_of(const Client *c, const char *spell) {
    for (int i = 0; i < c->backpack_count; i++) {
        const ItemTemplate *t = &item_database[c->backpack[i].template_idx];
        for (int k = 0; t->category == ITEM_BOOK && k < t->book_spell_count; k++) {
            if (strcmp(t->book_spell_names[k], spell) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool slot_for_level(const Client *c, int level) {
    for (int s = level; s <= MAX_SPELL_LEVEL && level > 0; s++) {
        if (c->spell_slots[s] > 0) {
            return true;
        }
    }
    return level == 0;
}

/*The best spell of a kind it can cast now (highest level, then dice), or
 * -1. range: the target's distance (0: on itself).*/
static int best_spell(const Client *c, SpellKind kind, SpellHandler handler, int range) {
    int best = -1;
    double best_score = -1.0;
    for (int i = 0; i < spell_database_size; i++) {
        const SpellTemplate *sp = &spell_database[i];
        const SpellEffect *e = &sp->effect;
        if (e->kind != kind || (kind == SPELL_KIND_SPECIAL && e->handler != handler) ||
            !spell_known_get(c, i) || !slot_for_level(c, sp->level) ||
            (range > 0 && e->range < range) || !carries_book_of(c, sp->name)) {
            continue;
        }
        double score =
            sp->level * 100.0 + e->dice_count * (e->dice_sides + 1) / 2.0 + e->dice_bonus;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int count_items(const Client *c, const char *part) {
    int n = 0;
    for (int i = 0; i < c->backpack_count; i++) {
        if (strcasestr(item_database[c->backpack[i].template_idx].name, part)) {
            n += c->backpack[i].stack_count > 0 ? c->backpack[i].stack_count : 1;
        }
    }
    return n;
}

/*The bot followed by DGL_CAMPAIGN_TRACE (its slot).*/
static bool traced(const Bot *b) {
    static int slot = -2;
    if (slot == -2) {
        const char *env = getenv("DGL_CAMPAIGN_TRACE");
        slot = env ? str_to_int(env) : -1;
    }
    return (int)(b - g_bots) == slot;
}

static void command(Bot *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void command(Bot *b, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    handle_text_cmd(b->c, line, &g_game);
    if (traced(b)) { /*what went wrong, for DGL_CAMPAIGN_TRACE*/
        static char out[1 << 16];
        size_t n = fx_output((int)(b - g_bots), out, sizeof(out) - 1);
        const char *err = NULL; /*the text sits among binary frames*/
        for (size_t k = 0; k + 7 <= n && !err; k++) {
            err = memcmp(out + k, "[ERROR]", 7) == 0 ? out + k : NULL;
        }
        if (err) {
            fprintf(stderr, "round %d '%s': %.*s\n", g_round, line,
                    (int)strnlen(err, (size_t)(out + n - err)), err);
        }
    }
}

/* --- the way: a breadth-first search from the character ---------------------------- */

static int g_from[MAP_WIDTH * MAP_HEIGHT];
static int g_queue[MAP_WIDTH * MAP_HEIGHT];
static unsigned g_seen_mark[MAP_WIDTH * MAP_HEIGHT];
static unsigned g_mark;

typedef struct {
    Goal goal;
    const Bot *b;
} Want;

static bool is_goal(const Want *w, int x, int y) {
    const Client *c = w->b->c;
    VoxelType v = g_game.world->floors[c->floor_id].map.data[0][y][x];
    NPC *n = npc_at(c->floor_id, x, y);
    switch (w->goal) {
    case GOAL_STAIRS:
        return v == VOXEL_STAIRS_DOWN;
    case GOAL_HUNT: /*not a foe its blows do not harm*/
        return n && hostile(n, c) && n->archetype != ARCH_BOSS &&
               (n->entity_id != w->b->foe_id || w->b->foe_rounds < 20);
    case GOAL_BOSS:
        return n && hostile(n, c) && n->archetype == ARCH_BOSS;
    case GOAL_GHOST:
        return n && n->is_ghost && ghost_is_of(n, c);
    case GOAL_UP:
        return v == VOXEL_STAIRS_UP;
    case GOAL_COUNT:
        break;
    }
    return false;
}

/*A gold pile or a treasure: walked onto, it is picked up.*/
static bool loot_at(int floor_id, int x, int y) {
    const NPC *n = npc_at(floor_id, x, y);
    return n && n->active && (n->archetype == ARCH_GOLD || n->archetype == ARCH_TREASURE);
}

/*First step towards the nearest goal, false if none is reached. Lava is
 * avoided while another way exists.*/
/*A search that finds nothing visits the whole floor twice: it is not
 * repeated on the same floor for MISS_ROUNDS.*/
#define MISS_ROUNDS 25

static bool first_step(Bot *b, Goal goal, int *dx, int *dy, int *dist) {
    const Client *c = b->c;
    const Floor *fl = &g_game.world->floors[c->floor_id];
    if (b->miss_floor[goal] == c->floor_id && g_round - b->miss_round[goal] < MISS_ROUNDS) {
        return false;
    }
    Want w = {goal, b};
    bool lava_seen = false;
    for (int pass = 0; pass < 2 && (pass == 0 || lava_seen); pass++) {
        g_mark++;
        int head = 0, tail = 0, start = c->y * MAP_WIDTH + c->x, found = -1;
        g_seen_mark[start] = g_mark;
        g_from[start] = -1;
        g_queue[tail++] = start;
        while (head < tail && found < 0) {
            int at = g_queue[head++];
            int x = at % MAP_WIDTH, y = at / MAP_WIDTH;
            static const int k_dx[4] = {1, -1, 0, 0}, k_dy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4 && found < 0; d++) {
                int nx = x + k_dx[d], ny = y + k_dy[d], ni = ny * MAP_WIDTH + nx;
                if (nx < 1 || ny < 1 || nx >= MAP_WIDTH - 1 || ny >= MAP_HEIGHT - 1 ||
                    g_seen_mark[ni] == g_mark) {
                    continue;
                }
                VoxelType v = fl->map.data[0][ny][nx];
                lava_seen |= v == VOXEL_LAVA;
                if (!voxel_passable(v) || (pass == 0 && v == VOXEL_LAVA) ||
                    (b->blocked_floor == c->floor_id && b->blocked[ny][nx])) {
                    continue;
                }
                g_seen_mark[ni] = g_mark;
                g_from[ni] = at;
                if (is_goal(&w, nx, ny)) {
                    found = ni;
                } else if (fl->entity_grid[ny][nx] == 0 || loot_at(c->floor_id, nx, ny)) {
                    g_queue[tail++] = ni; /*loot is picked up on the way*/
                }
            }
        }
        if (found >= 0) {
            int steps = 1, at = found;
            while (g_from[at] != start) {
                at = g_from[at];
                steps++;
            }
            *dx = at % MAP_WIDTH - c->x;
            *dy = at / MAP_WIDTH - c->y;
            *dist = steps;
            return true;
        }
    }
    b->miss_floor[goal] = c->floor_id;
    b->miss_round[goal] = g_round;
    return false;
}

/*Steps to the stairs down of each floor, from every tile (walls and rock
 * excluded; lava does not burn a character; 0xFFFF: no way), computed once
 * per floor.*/
static uint16_t *g_to_stairs[MAX_FLOORS];

static const uint16_t *stairs_field(int f) {
    if (g_to_stairs[f]) {
        return g_to_stairs[f];
    }
    uint16_t *d = malloc(sizeof(uint16_t) * MAP_WIDTH * MAP_HEIGHT);
    if (!d) {
        return NULL;
    }
    memset(d, 0xFF, sizeof(uint16_t) * MAP_WIDTH * MAP_HEIGHT);
    const Floor *fl = &g_game.world->floors[f];
    int head = 0, tail = 0;
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            if (fl->map.data[0][y][x] == VOXEL_STAIRS_DOWN) {
                d[y * MAP_WIDTH + x] = 0;
                g_queue[tail++] = y * MAP_WIDTH + x;
            }
        }
    }
    while (head < tail) {
        int at = g_queue[head++];
        int x = at % MAP_WIDTH, y = at / MAP_WIDTH;
        static const int k_dx[4] = {1, -1, 0, 0}, k_dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = x + k_dx[k], ny = y + k_dy[k], ni = ny * MAP_WIDTH + nx;
            if (nx < 1 || ny < 1 || nx >= MAP_WIDTH - 1 || ny >= MAP_HEIGHT - 1 ||
                d[ni] != 0xFFFF) {
                continue;
            }
            if (!voxel_passable(fl->map.data[0][ny][nx])) {
                continue;
            }
            d[ni] = (uint16_t)(d[at] + 1);
            g_queue[tail++] = ni;
        }
    }
    g_to_stairs[f] = d;
    return d;
}

/*The step down the field of the stairs: the free neighbour nearest to
 * them, or a monster in the way (to fight it). False if there is none.*/
static bool stairs_step(const Bot *b, int *dx, int *dy) {
    const Client *c = b->c;
    const uint16_t *d = stairs_field(c->floor_id);
    if (!d) {
        return false;
    }
    int best = d[c->y * MAP_WIDTH + c->x];
    bool found = false;
    static const int k_dx[4] = {1, -1, 0, 0}, k_dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
        int nx = c->x + k_dx[k], ny = c->y + k_dy[k];
        int v = d[ny * MAP_WIDTH + nx];
        int id = g_game.world->floors[c->floor_id].entity_grid[ny][nx];
        NPC *n = id > 0 ? npc_by_entity_id(&g_game, id) : NULL;
        bool free = id == 0 || (n && hostile(n, c)) || loot_at(c->floor_id, nx, ny);
        if (v < best && free && !(b->blocked_floor == c->floor_id && b->blocked[ny][nx])) {
            best = v;
            *dx = k_dx[k];
            *dy = k_dy[k];
            found = true;
        }
    }
    return found;
}

/*Moves one step; a step that does not move (something in the way that is
 * no monster) blocks that tile for a while.*/
static void step(Bot *b, int dx, int dy) {
    Client *c = b->c;
    int ox = c->x, oy = c->y, of = c->floor_id;
    NPC *n = npc_at(of, ox + dx, oy + dy);
    bool attack = n && hostile(n, c);
    c->last_action_ms = 0; /*one step a round, the pace of the server*/
    MsgMove m = {dx, dy};
    player_move(c, &m, g_game.clients, &g_game);
    if (!attack && c->x == ox && c->y == oy && c->floor_id == of && !npc_at(of, ox + dx, oy + dy)) {
        if (b->blocked_floor != of) {
            memset(b->blocked, 0, sizeof(b->blocked));
            b->blocked_floor = of;
        }
        b->blocked[oy + dy][ox + dx] = 1;
    }
}

/* --- town ---------------------------------------------------------------------------- */

/*Next to a merchant of the town, the shelf of the item asked (-1: none).*/
static NPC *merchant_with(const Client *c, const char *name, int *shelf, uint64_t *price) {
    for (int i = 0; i < MAX_NPCS; i++) {
        NPC *n = &g_game.npcs[i];
        if (!n->active || n->floor_id != 0 || n->archetype != ARCH_MERCHANT) {
            continue;
        }
        const MerchantData *m = npc_shop_any(&g_game.pools, n);
        for (int j = 0; m && j < m->item_count; j++) {
            if (m->item_stock[j] > 0 &&
                strcmp(item_database[m->item_templates[j]].name, name) == 0) {
                *shelf = j;
                *price = item_database[m->item_templates[j]].cost * 2; /*an upper bound*/
                (void)c;
                return n;
            }
        }
    }
    return NULL;
}

static void stand_by(Client *c, const NPC *n) {
    Floor *fl = &g_game.world->floors[0];
    int x = n->x, y = n->y;
    find_walkable_near(fl, n->x + 1, n->y, &x, &y);
    if (abs(x - n->x) > 1 || abs(y - n->y) > 1) {
        x = n->x + 1;
        y = n->y;
    }
    if (fl->entity_grid[c->y][c->x] == c->entity_id) {
        fl->entity_grid[c->y][c->x] = 0;
    }
    c->x = x;
    c->y = y;
}

static bool buy(Bot *b, const char *name) {
    int shelf = -1;
    uint64_t price = 0;
    NPC *n = merchant_with(b->c, name, &shelf, &price);
    if (!n || b->c->gold < price || b->c->backpack_count >= MAX_BACKPACK - 2) {
        return false;
    }
    stand_by(b->c, n);
    uint64_t before = b->c->gold;
    command(b, "buy %d", shelf + 1);
    return b->c->gold < before;
}

/*Everything of the backpack it does not use goes to a merchant.*/
static void sell_loot(Bot *b) {
    Client *c = b->c;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].floor_id == 0 &&
            g_game.npcs[i].archetype == ARCH_MERCHANT) {
            stand_by(c, &g_game.npcs[i]);
            break;
        }
    }
    /*What it uses stays: the books of its class, healing, food, light and
     * arrows. The rest is sold, or left on the floor of the town when no
     * merchant takes it (unidentified).*/
    for (int i = c->backpack_count - 1; i >= 0; i--) {
        const ItemTemplate *t = &item_database[c->backpack[i].template_idx];
        bool first = true; /*one copy of a book is enough*/
        for (int j = 0; j < i && first; j++) {
            first = c->backpack[j].template_idx != c->backpack[i].template_idx;
        }
        if ((t->category == ITEM_BOOK && first &&
             (t->book_class_mask & (1u << (int)c->class_id))) ||
            strcmp(t->name, "Potion of Healing") == 0 ||
            (t->category == ITEM_AMMO && c->class_id == CLASS_RANGER) ||
            t->category == ITEM_LIGHT_SOURCE || t->category == ITEM_FUEL || item_is_food(t)) {
            continue;
        }
        int before = c->backpack_count;
        command(b, "sell %d", i + 1);
        if (c->backpack_count == before) {
            command(b, "drop %d", i + 1);
        }
    }
}

static double weapon_value(const ItemTemplate *t) {
    return t->damage_dice_count * (t->damage_dice_sides + 1) / 2.0 + t->attack_bonus;
}

/*The best of a category it can afford, better than what it wears.*/
static void upgrade(Bot *b, ItemCategory cat, double now, double (*value)(const ItemTemplate *)) {
    Client *c = b->c;
    int best = -1;
    double best_v = now;
    for (int i = 0; i < item_database_size; i++) {
        const ItemTemplate *t = &item_database[i];
        if (t->category != cat || t->cost == 0 || t->cost * 2 > c->gold / 2 ||
            t->ammo != AMMO_NONE || value(t) <= best_v) {
            continue;
        }
        int shelf;
        uint64_t price;
        if (merchant_with(c, t->name, &shelf, &price)) {
            best = i;
            best_v = value(t);
        }
    }
    if (best >= 0 && buy(b, item_database[best].name)) {
        command(b, "wear %s", item_database[best].name);
    }
}

static double armour_value(const ItemTemplate *t) {
    return t->ac_base + t->ac_bonus;
}

static bool melee_class(ClassType cls) {
    return cls == CLASS_BARBARIAN || cls == CLASS_FIGHTER || cls == CLASS_PALADIN ||
           cls == CLASS_ROGUE || cls == CLASS_MONK || cls == CLASS_CLERIC;
}

/*The armour and weapons of its loot: identified at a merchant (100 gold)
 * and worn when better than what it wears, as a player does.*/
static void wear_loot(Bot *b) {
    Client *c = b->c;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].floor_id == 0 &&
            g_game.npcs[i].archetype == ARCH_MERCHANT) {
            stand_by(c, &g_game.npcs[i]);
            break;
        }
    }
    for (int i = 0; i < c->backpack_count; i++) {
        const ItemInstance *it = &c->backpack[i];
        const ItemTemplate *t = &item_database[it->template_idx];
        bool armour = t->category == ITEM_ARMOR;
        bool weapon =
            t->category == ITEM_WEAPON && melee_class(c->class_id) && t->ammo == AMMO_NONE;
        if ((!armour && !weapon) || c->gold < 2000) {
            continue;
        }
        if (!it->is_identified) {
            command(b, "identify %d", i + 1);
        }
        const ItemInstance *worn = armour ? &c->slot_body : &c->slot_hand_r;
        double now = 0, loot = 0;
        if (armour) {
            now = worn->template_idx >= 0
                      ? armour_value(&item_database[worn->template_idx]) + worn->ac_bonus
                      : 0;
            loot = armour_value(t) + it->ac_bonus;
        } else {
            now = worn->template_idx >= 0 ? weapon_value(&item_database[worn->template_idx]) +
                                                worn->to_hit_bonus + worn->to_dam_bonus
                                          : 0;
            loot = weapon_value(t) + it->to_hit_bonus + it->to_dam_bonus;
        }
        if (it->is_identified && loot > now) {
            command(b, "wear %d", i + 1);
        }
    }
}

static void shop(Bot *b) {
    Client *c = b->c;
    wear_loot(b);
    sell_loot(b); /*the rest, and what it wore before*/
    while (count_items(c, "Potion of Healing") < 6 && buy(b, "Potion of Healing")) {
    }
    while (count_items(c, "Rations") < 4 && buy(b, "Rations (1 day)")) {
    }
    if (count_items(c, "Torch") + count_items(c, "Lantern") == 0 && buy(b, "Torch")) {
        command(b, "belt Torch");
    }
    if (c->class_id == CLASS_RANGER) {
        while (count_items(c, "Arrows") < 60 && buy(b, "Arrows (20)")) {
        }
        command(b, "belt Arrows");
    }
    const ItemInstance *body = &c->slot_body;
    upgrade(b, ITEM_ARMOR,
            body->template_idx >= 0 ? armour_value(&item_database[body->template_idx]) : 0,
            armour_value);
    if (melee_class(c->class_id)) {
        const ItemInstance *w = &c->slot_hand_r;
        upgrade(b, ITEM_WEAPON,
                w->template_idx >= 0 ? weapon_value(&item_database[w->template_idx]) : 0,
                weapon_value);
    }
    /*The next book of its class (the cheapest it lacks): the books are
     * carried to cast their spells, room for food and potions stays.*/
    int books = 0;
    for (int i = 0; i < c->backpack_count; i++) {
        books += item_database[c->backpack[i].template_idx].category == ITEM_BOOK;
    }
    for (int i = 0; i < item_database_size && books < 6 && c->backpack_count < MAX_BACKPACK - 8;
         i++) {
        const ItemTemplate *t = &item_database[i];
        if (t->category == ITEM_BOOK && (t->book_class_mask & (1u << (int)c->class_id)) &&
            count_items(c, t->name) == 0 && t->cost * 2 < c->gold / 2 && buy(b, t->name)) {
            books++;
        }
    }
    if (c->hunger_level > 800) {
        command(b, "eat");
    }
}

static void study(Bot *b) {
    Client *c = b->c;
    for (int t = 0; t < TEMPLE_COUNT; t++) {
        if (TEMPLES[t].cls != (int)c->class_id) {
            continue;
        }
        c->x = (TEMPLES[t].x0 + TEMPLES[t].x1) / 2;
        c->y = (TEMPLES[t].y0 + TEMPLES[t].y1) / 2;
        for (int i = 0; i < c->backpack_count; i++) {
            if (item_database[c->backpack[i].template_idx].category == ITEM_BOOK) {
                command(b, "%s %d", TEMPLES[t].ritual, i + 1);
            }
        }
    }
    c->needs_study = false;
}

static const char *const k_primary[CLASS_COUNT] = {
    [CLASS_BARBARIAN] = "str", [CLASS_BARD] = "cha",    [CLASS_CLERIC] = "wis",
    [CLASS_DRUID] = "wis",     [CLASS_FIGHTER] = "str", [CLASS_MONK] = "dex",
    [CLASS_PALADIN] = "str",   [CLASS_RANGER] = "dex",  [CLASS_ROGUE] = "dex",
    [CLASS_SORCERER] = "cha",  [CLASS_WARLOCK] = "cha", [CLASS_WIZARD] = "int",
};

/*Down: the transit spell to the deepest floor seen, else the stairs.*/
static void go_down(Bot *b) {
    Client *c = b->c;
    int transit = best_spell(c, SPELL_KIND_SPECIAL, SPELL_HANDLER_CLASS_TRANSIT, 0);
    if (transit >= 0 && c->max_floor_explored > 0) {
        command(b, "cast %s", spell_database[transit].name);
    }
    if (c->floor_id == 0) {
        Floor *fl = &g_game.world->floors[0];
        if (fl->entity_grid[c->y][c->x] == c->entity_id) {
            fl->entity_grid[c->y][c->x] = 0;
        }
        c->x = MAP_CENTER_X + 1;
        c->y = MAP_CENTER_Y;
        step(b, -1, 0);
    }
    b->errands_done = false;
}

static void town(Bot *b) {
    Client *c = b->c;
    if (b->errands_done) {
        go_down(b);
        return;
    }
    command(b, "rest");
    for (int k = 0; c->unspent_stat_points > 0 && k < 60; k++) {
        command(b, "train %s", k % 3 == 2 ? "con" : k_primary[c->class_id]);
    }
    if (c->needs_study) {
        study(b);
    }
    shop(b);
    b->busy = TOWN_ROUNDS;
    b->errands_done = true;
}

/* --- the dungeon ------------------------------------------------------------------------- */

static int belt_ammo(const Client *c) {
    int n = 0;
    for (int i = 0; i < MAX_BELT; i++) {
        int t = c->belt[i].template_idx;
        if (t >= 0 && item_database[t].category == ITEM_AMMO) {
            n += c->belt[i].stack_count;
        }
    }
    return n;
}

/*Back to town when it can no longer heal; true if it went.*/
static bool look_after(Bot *b) {
    Client *c = b->c;
    if (c->hunger_level > 1300 && count_items(c, "Rations") > 0) {
        command(b, "eat");
    }
    int transit = best_spell(c, SPELL_KIND_SPECIAL, SPELL_HANDLER_CLASS_TRANSIT, 0);
    if (c->hunger_level > 1600 && count_items(c, "Rations") == 0 && transit >= 0) {
        command(b, "cast %s", spell_database[transit].name); /*food in town*/
        return c->floor_id == 0;
    }
    if (c->class_id == CLASS_RANGER && belt_ammo(c) == 0 && count_items(c, "Arrows") == 0 &&
        c->gold >= 100 && transit >= 0) {                    /*with the money to buy them*/
        command(b, "cast %s", spell_database[transit].name); /*arrows in town*/
        return c->floor_id == 0;
    }
    if (c->backpack_count >= MAX_BACKPACK - 1 && transit >= 0) {
        command(b, "cast %s", spell_database[transit].name); /*the loot to sell*/
        return c->floor_id == 0;
    }
    if (c->hp * 2 >= c->max_hp) {
        return false;
    }
    if (count_items(c, "Potion of Healing") > 0) {
        command(b, "use Potion of Healing");
        return false;
    }
    int heal = best_spell(c, SPELL_KIND_HEAL, SPELL_HANDLER_NONE, 0);
    if (heal >= 0) {
        command(b, "cast %s", spell_database[heal].name);
        return false;
    }
    if (c->hp * 10 < c->max_hp * 3 && transit >= 0) {
        command(b, "cast %s", spell_database[transit].name);
        return c->floor_id == 0;
    }
    return false;
}

/*A shot or a spell at a monster within reach of it.*/
static bool attack_from_afar(Bot *b, int dist) {
    Client *c = b->c;
    if (c->class_id == CLASS_RANGER && dist <= 8) {
        int before = belt_ammo(c);
        command(b, "shoot");
        if (belt_ammo(c) != before) {
            return true;
        }
    }
    int sp = best_spell(c, SPELL_KIND_DAMAGE, SPELL_HANDLER_NONE, dist);
    if (sp < 0 || dist > 8) {
        return false;
    }
    int slots = 0;
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
        slots += c->spell_slots[s];
    }
    command(b, "cast %s", spell_database[sp].name);
    int after = 0;
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
        after += c->spell_slots[s];
    }
    return spell_database[sp].level == 0 || after < slots;
}

static bool ghost_here(const Client *c) {
    FloorIter walk = floor_iter(&g_game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (g_game.npcs[i].is_ghost && ghost_is_of(&g_game.npcs[i], c)) {
            return true;
        }
    }
    return false;
}

static void dungeon(Bot *b) {
    Client *c = b->c;
    if (look_after(b)) {
        return;
    }
    int dx = 0, dy = 0, dist = 0;
    int f = c->floor_id;
    bool boss_gate = f % 10 == 0 && !(c->bosses_defeated & (1u << (f / 10 - 1)));
    /*The level it hunts for before going down: three more before a boss.*/
    int wanted = 1 + (f + 1) / 5 + ((f + 1) % 10 == 0 || boss_gate ? 3 : 0);
    wanted = wanted > 20 ? 20 : wanted;
    int transit = best_spell(c, SPELL_KIND_SPECIAL, SPELL_HANDLER_CLASS_TRANSIT, 0);
    if (c->needs_study && transit >= 0) {
        command(b, "cast %s", spell_database[transit].name); /*new spells to learn*/
        if (c->floor_id == 0) {
            return;
        }
    }
    if (ghost_here(c) && first_step(b, GOAL_GHOST, &dx, &dy, &dist)) {
        step(b, dx, dy);
        return;
    }
    Goal goal = GOAL_STAIRS;
    if (boss_gate && c->level < wanted) {
        goal = GOAL_UP; /*too weak for the boss: back up, to hunt*/
    } else if (boss_gate) {
        goal = GOAL_BOSS;
    } else if (c->level < wanted) {
        goal = GOAL_HUNT;
    }
    bool found = goal != GOAL_STAIRS && first_step(b, goal, &dx, &dy, &dist);
    if (!found) {
        goal = GOAL_STAIRS;
        found = stairs_step(b, &dx, &dy);
    }
    /*What is next to it first, then what is near. A foe its blows do not
     * harm (immune to weapons) gets a spell, or is left alone: its tile is
     * avoided.*/
    for (int d = 0; d < 4; d++) {
        static const int k_dx[4] = {1, -1, 0, 0}, k_dy[4] = {0, 0, 1, -1};
        NPC *n = npc_at(f, c->x + k_dx[d], c->y + k_dy[d]);
        if (!n || !hostile(n, c)) {
            continue;
        }
        if (n->entity_id == b->foe_id && n->hp >= b->foe_hp) {
            b->foe_rounds++;
        } else {
            b->foe_id = n->entity_id;
            b->foe_rounds = 0;
        }
        b->foe_hp = n->hp;
        if (b->foe_rounds < 20) {
            step(b, k_dx[d], k_dy[d]);
            return;
        }
        if (attack_from_afar(b, 1)) {
            return;
        }
        if (b->blocked_floor != f) {
            memset(b->blocked, 0, sizeof(b->blocked));
            b->blocked_floor = f;
        }
        b->blocked[n->y][n->x] = 1;
    }
    if (found && goal != GOAL_STAIRS && dist > 1 && attack_from_afar(b, dist)) {
        return;
    }
    if (found) {
        step(b, dx, dy);
    } else {
        static const int k_dx[4] = {1, -1, 0, 0}, k_dy[4] = {0, 0, 1, -1};
        int d = rand() % 4;
        step(b, k_dx[d], k_dy[d]);
    }
}

/* --- the bots ------------------------------------------------------------------------- */

static void note_progress(Bot *b) {
    Client *c = b->c;
    if (c->floor_id > b->deepest) {
        b->deepest = c->floor_id;
        b->progress_round = g_round;
        if (b->floor_round[c->floor_id] == 0) {
            b->floor_round[c->floor_id] = g_round;
            b->floor_level[c->floor_id] = c->level;
        }
    }
    if (c->level > b->best_level) {
        b->best_level = c->level;
        b->progress_round = g_round;
    }
    if (client_conquered(c) && b->conquered_round == 0) {
        b->conquered_round = g_round;
    }
    if (!b->conquered_round && !b->stalled_round && g_round - b->progress_round > STALL_ROUNDS) {
        b->stalled_round = g_round;
        snprintf(b->stall_where, sizeof(b->stall_where), "floor %d (%d,%d), level %d, HP %d/%d",
                 c->floor_id, c->x, c->y, c->level, c->hp, c->max_hp);
    }
}

static void trace(const Bot *b) {
    const Client *c = b->c;
    if (!traced(b) || g_round % 2000 != 0) {
        return;
    }
    const uint16_t *d = c->floor_id > 0 ? stairs_field(c->floor_id) : NULL;
    fprintf(stderr,
            "trace %d: floor %d (%d,%d) to stairs %d, level %d xp %d HP %d/%d hunger %d gold %lu "
            "backpack %d busy %d errands %d\n",
            g_round, c->floor_id, c->x, c->y, d ? d[c->y * MAP_WIDTH + c->x] : -1, c->level, c->xp,
            c->hp, c->max_hp, c->hunger_level, (unsigned long)c->gold, c->backpack_count, b->busy,
            b->errands_done);
    if (getenv("DGL_CAMPAIGN_NPCS")) { /*the table: who holds the slots*/
        int by[ARCH_COUNT + 1] = {0}, dead = 0, bosses = 0;
        for (int i = 0; i < MAX_NPCS; i++) {
            const NPC *n = &g_game.npcs[i];
            by[n->active ? (int)n->archetype : ARCH_COUNT]++;
            dead += !n->active && n->template && n->respawn_timer >= 0;
            bosses += n->archetype == ARCH_BOSS && !n->is_ghost;
        }
        fprintf(stderr,
                "  npcs: gold %d treasure %d merchants %d inactive %d (dead %d) bosses %d\n",
                by[ARCH_GOLD], by[ARCH_TREASURE], by[ARCH_MERCHANT], by[ARCH_COUNT], dead, bosses);
    }
    if (getenv("DGL_CAMPAIGN_MAP") && d) { /*voxel/steps to the stairs, around it*/
        for (int y = c->y - 4; y <= c->y + 4; y++) {
            fprintf(stderr, "  ");
            for (int x = c->x - 6; x <= c->x + 6; x++) {
                if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) {
                    continue;
                }
                int id = g_game.world->floors[c->floor_id].entity_grid[y][x];
                const NPC *n = npc_at(c->floor_id, x, y);
                char who = 'p'; /*@ the bot, M a monster, n another NPC, p a character*/
                if (x == c->x && y == c->y) {
                    who = '@';
                } else if (id == 0) {
                    who = ' ';
                } else if (n) {
                    who = hostile(n, c) ? 'M' : 'n';
                }
                fprintf(stderr, "%c%02d/%-5d", who,
                        g_game.world->floors[c->floor_id].map.data[0][y][x], d[y * MAP_WIDTH + x]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (getenv("DGL_CAMPAIGN_PACK")) {
        for (int i = 0; i < c->backpack_count; i++) {
            fprintf(stderr, "  [%d] %s x%d%s\n", i + 1,
                    item_database[c->backpack[i].template_idx].name, c->backpack[i].stack_count,
                    c->backpack[i].is_identified ? "" : " (unknown)");
        }
    }
    if (c->floor_id % 10 == 0 && c->floor_id > 0) {
        for (int i = 0; i < MAX_NPCS; i++) {
            const NPC *n = &g_game.npcs[i];
            if (n->archetype == ARCH_BOSS && n->floor_id == c->floor_id && !n->is_ghost) {
                fprintf(stderr, "  boss active %d at (%d,%d) spawn (%d,%d) timer %d HP %d/%d\n",
                        n->active, n->x, n->y, n->spawn_x, n->spawn_y, n->respawn_timer, n->hp,
                        n->max_hp);
                int id = g_game.world->floors[c->floor_id].entity_grid[n->spawn_y][n->spawn_x];
                const NPC *o = npc_at(c->floor_id, n->spawn_x, n->spawn_y);
                fprintf(stderr, "  spawn tile: grid %d voxel %d npc %s arch %d active %d\n", id,
                        g_game.world->floors[c->floor_id].map.data[0][n->spawn_y][n->spawn_x],
                        o && o->template ? o->template->name : "-", o ? (int)o->archetype : -1,
                        o ? o->active : -1);
            }
        }
    }
}

static void play(Bot *b) {
    Client *c = b->c;
    trace(b);
    if (b->conquered_round || b->stalled_round) {
        return;
    }
    if (b->busy > 0) {
        b->busy--;
    } else if (c->floor_id == 0) {
        town(b);
    } else {
        dungeon(b);
    }
    note_progress(b);
}

static Client *create(int slot, ClassType cls) {
    char name[16];
    snprintf(name, sizeof(name), "bot%02d", slot);
    Client *c = fx_player(slot, name);
    if (!c) {
        return NULL;
    }
    c->class_id = cls;
    c->race_id = 0;
    c->subrace_id = -1;
    /*The standard array: 15 in the ability of the class, 14 in CON.*/
    c->str = c->dex = c->intel = c->wis = c->cha = 10;
    c->con = 14;
    const char *p = k_primary[cls];
    int *primary = &c->cha;
    if (strcmp(p, "str") == 0) {
        primary = &c->str;
    } else if (strcmp(p, "dex") == 0) {
        primary = &c->dex;
    } else if (strcmp(p, "int") == 0) {
        primary = &c->intel;
    } else if (strcmp(p, "wis") == 0) {
        primary = &c->wis;
    }
    *primary = 15;
    c->hp = c->max_hp = 20 + rules_get_modifier(c->con);
    c->gold = 1000;
    class_spell_slots(cls, 1, c->spell_slots_max);
    memcpy(c->spell_slots, c->spell_slots_max, sizeof(c->spell_slots));
    give_starting_gear(c);
    char wear[8][96];
    int n = 0;
    for (int i = 0; i < c->backpack_count && n < 8; i++) {
        ItemCategory cat = item_database[c->backpack[i].template_idx].category;
        if (cat == ITEM_WEAPON || cat == ITEM_ARMOR || cat == ITEM_SHIELD) {
            snprintf(wear[n++], sizeof(wear[0]), "wear %s",
                     item_database[c->backpack[i].template_idx].name);
        }
    }
    for (int i = 0; i < n; i++) {
        handle_text_cmd(c, wear[i], &g_game);
    }
    c->floor_id = 0;
    c->x = MAP_CENTER_X + 1;
    c->y = MAP_CENTER_Y + 1;
    return c;
}

static FILE *g_out; /*the report: stdout itself carries the server log*/

static void report(FILE *json, int limit) {
    fprintf(g_out, "%-10s %5s %5s %6s %9s  %s\n", "class", "floor", "level", "deaths", "rounds",
            "rounds to floor 10/20/.../100 (level)");
    if (json) {
        fprintf(json, "{\n  \"rounds\": %d,\n  \"classes\": [\n", g_round);
    }
    for (int i = 0; i < BOTS; i++) {
        Bot *b = &g_bots[i];
        char tens[256] = "";
        for (int f = 10; f < MAX_FLOORS; f += 10) {
            char one[24];
            if (b->floor_round[f]) {
                snprintf(one, sizeof(one), " %d(%d)", b->floor_round[f], b->floor_level[f]);
            } else {
                snprintf(one, sizeof(one), " -");
            }
            strncat(tens, one, sizeof(tens) - strlen(tens) - 1);
        }
        const char *end = "time";
        int last = limit;
        if (b->conquered_round) {
            end = "CONQUERED";
            last = b->conquered_round;
        } else if (b->stalled_round) {
            end = "STALLED";
            last = b->stalled_round;
        }
        fprintf(g_out, "%-10s %5d %5d %6d %9d  %s  [%s%s%s]\n", CLASSES[b->c->class_id].name,
                b->deepest, b->c->level, b->deaths, last, tens, end, b->stalled_round ? ": " : "",
                b->stall_where);
        if (json) {
            fprintf(json,
                    "    {\"class\": \"%s\", \"deepest\": %d, \"level\": %d, \"deaths\": %d, "
                    "\"end\": \"%s\", \"round\": %d}%s\n",
                    CLASSES[b->c->class_id].name, b->deepest, b->c->level, b->deaths, end,
                    b->conquered_round ? b->conquered_round : b->stalled_round,
                    i + 1 < BOTS ? "," : "");
        }
    }
    if (json) {
        fprintf(json, "  ]\n}\n");
    }
}

int main(int argc, char **argv) {
    int limit = argc > 1 ? str_to_int(argv[1]) : DEFAULT_LIMIT;
    unsigned seed = argc > 2 ? (unsigned)str_to_int(argv[2]) : 20261121u;
    FILE *json = argc > 3 ? fopen(argv[3], "w") : NULL;
    /*The server log goes to stdout, a line per event: thousands per round
     * with nine characters. It is dropped unless DGL_CAMPAIGN_LOG is set;
     * the report goes to the original stdout.*/
    g_out = stdout;
    if (!getenv("DGL_CAMPAIGN_LOG")) {
        int fd = dup(STDOUT_FILENO);
        FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (out && freopen("/dev/null", "w", stdout)) {
            g_out = out;
        }
    }
    if (fx_init("state-campaign", DGL_TEST_DATA_DIR) != 0) {
        return 2;
    }
    srand(seed);
    generate_world(&g_game);
    floor_index_rebuild(&g_game);
    for (int i = 0; i < BOTS; i++) {
        g_bots[i].c = create(i, (ClassType)i);
        if (!g_bots[i].c) {
            fprintf(stderr, "campaign_sim: cannot create the character of class %d\n", i);
            return 2;
        }
        g_bots[i].best_level = 1;
        g_bots[i].blocked_floor = -1;
        for (int g = 0; g < GOAL_COUNT; g++) {
            g_bots[i].miss_floor[g] = -1;
        }
    }
    int done = 0;
    for (g_round = 1; g_round <= limit && done < BOTS; g_round++) {
        done = 0;
        for (int i = 0; i < BOTS; i++) {
            play(&g_bots[i]);
            fx_drain(i);
            done += g_bots[i].conquered_round || g_bots[i].stalled_round;
        }
        update_world(g_game.clients, &g_game);
        for (int i = 0; i < BOTS; i++) {
            fx_drain(i);
            Bot *b = &g_bots[i];
            Client *c = b->c;
            if (c->floor_id == 0 && b->last_floor > 0 && c->backpack_count == 0 && c->gold == 0) {
                b->deaths++; /*back in town with nothing: the bones stayed below*/
                b->errands_done = false;
            }
            b->last_floor = c->floor_id;
        }
        if (g_round % 50000 == 0) {
            fprintf(stderr, "campaign_sim: round %d:", g_round);
            for (int i = 0; i < BOTS; i++) {
                fprintf(stderr, " %.3s %d/%d", CLASSES[g_bots[i].c->class_id].name,
                        g_bots[i].deepest, g_bots[i].c->level);
            }
            fprintf(stderr, "\n");
        }
    }
    report(json, limit);
    if (json) {
        fclose(json);
    }
    fflush(g_out);
    for (int i = 0; i < BOTS; i++) {
        if (g_bots[i].conquered_round) {
            return 0;
        }
    }
    return 1;
}
