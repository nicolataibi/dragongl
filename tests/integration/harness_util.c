/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Helpers shared by the harness modules (see harness_util.h).
 */

#include "harness_util.h"

#include "aoe.h"
#include "items.h"
#include "map.h"
#include "server_commands.h"
#include "server_entities.h"
#include "server_internal.h"

#include <dirent.h>
#include <string.h>
#include <unistd.h>

char hx_out[1 << 16];
size_t hx_out_len;

bool said(const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= hx_out_len; i++) {
        if (memcmp(hx_out + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

static int slot_of(const Client *c) {
    return (int)(c - g_test_clients);
}

void hush(const Client *c) {
    fx_drain(slot_of(c));
}

void heard(const Client *c) {
    hx_out_len = fx_output(slot_of(c), hx_out, sizeof(hx_out));
}

void run(Client *c, const char *line) {
    hush(c);
    handle_text_cmd(c, line, &g_game);
    heard(c);
}

static void open_floor(int floor) {
    Floor *fl = &g_game.world->floors[floor];
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            bool edge = x == 0 || y == 0 || x == MAP_WIDTH - 1 || y == MAP_HEIGHT - 1;
            fl->map.data[0][y][x] = edge ? VOXEL_ROCK : VOXEL_FLOOR;
            fl->entity_grid[y][x] = 0;
        }
    }
    fl->trap_count = 0;
    fl->crystal_respawn_count = 0;
}

void hx_reset_world(void) {
    memset(g_game.npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int i = 0; i < MAX_NPCS; i++) {
        g_game.npcs[i].respawn_timer = -1;
    }
    memset(g_game.tombstones, 0, sizeof(g_game.tombstones));
    static const int floors[] = {0, 1, 2, 10};
    for (size_t i = 0; i < sizeof(floors) / sizeof(floors[0]); i++) {
        open_floor(floors[i]);
    }
    aoe_init_clouds(&g_game);
    floor_index_rebuild(&g_game);
}

void place(Client *c, int floor, int x, int y) {
    Floor *old = &g_game.world->floors[c->floor_id];
    if (old->entity_grid[c->y][c->x] == c->entity_id) {
        old->entity_grid[c->y][c->x] = 0;
    }
    c->floor_id = floor;
    c->x = x;
    c->y = y;
    g_game.world->floors[floor].entity_grid[y][x] = c->entity_id;
}

Client *hero(int slot, const char *name, int floor, int x, int y) {
    static bool made[MAX_CLIENTS];
    Client *c = &g_test_clients[slot];
    if (!made[slot] || strcmp(c->username, name) != 0) {
        if (made[slot]) {
            fx_release(slot);
        }
        c = fx_player(slot, name);
        if (!c) {
            return NULL;
        }
        made[slot] = true;
    }
    c->is_dm = false;
    c->level = 5;
    c->xp = 0;
    c->str = c->dex = c->con = c->intel = c->wis = c->cha = 14;
    c->hp = c->max_hp = 60;
    c->gold = 1000;
    c->effect_count = 0;
    c->backpack_count = 0;
    c->party_leader[0] = '\0';
    for (int i = 0; i < MAX_BACKPACK; i++) {
        fx_reset_item(&c->backpack[i]);
    }
    for (int i = 0; i < MAX_BELT; i++) {
        fx_reset_item(&c->belt[i]);
    }
    for (int i = 0; i < 10; i++) {
        fx_reset_item(&c->slot_rings[i]);
    }
    ItemInstance *slots[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                             &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                             &c->slot_arm_l,  &c->slot_feet};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        fx_reset_item(slots[i]);
    }
    c->floor_id = floor;
    c->x = x;
    c->y = y;
    g_game.world->floors[floor].entity_grid[y][x] = c->entity_id;
    hush(c);
    return c;
}

NPC *monster(int slot, const char *name, int floor, int x, int y) {
    NPC *n = &g_game.npcs[slot];
    memset(n, 0, sizeof(*n));
    for (int i = 0; i < bestiary_size; i++) {
        if (!name || strcasestr(bestiary_data[i].name, name)) {
            n->template = &bestiary_data[i];
            n->template_idx = i;
            break;
        }
    }
    n->archetype = ARCH_MELEE;
    n->x = n->spawn_x = x;
    n->y = n->spawn_y = y;
    n->hp = n->max_hp = 30;
    n->ac = 10;
    n->attack_bonus = 2;
    n->damage_dice = 1;
    n->damage_sides = 6;
    n->xp_reward = 50;
    n->floor_id = floor;
    n->entity_id = 9000 + slot;
    n->respawn_timer = -1;
    n->ai_ctx.current_target_id = -1;
    npc_set_active(&g_game, n, true);
    g_game.world->floors[floor].entity_grid[y][x] = n->entity_id;
    floor_index_rebuild(&g_game);
    return n;
}

int item_named(const char *part) {
    for (int i = 0; i < item_database_size; i++) {
        if (strcasestr(item_database[i].name, part)) {
            return i;
        }
    }
    return -1;
}

int item_of(ItemCategory category) {
    for (int i = 0; i < item_database_size; i++) {
        if (item_database[i].category == category) {
            return i;
        }
    }
    return -1;
}

ItemInstance *give(Client *c, int template_idx, int count) {
    ItemInstance *it = &c->backpack[c->backpack_count++];
    fx_reset_item(it);
    it->template_idx = template_idx;
    it->stack_count = count;
    it->is_identified = true;
    it->blessing = BLESS_NORMAL; /* zero is BLESS_CURSED */
    it->quality = QUALITY_NORMAL;
    it->element = ELEM_NONE;
    it->durability = item_database[template_idx].max_durability > 0
                         ? item_database[template_idx].max_durability
                         : 100;
    return it;
}

bool file_with_prefix(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d)) != NULL;) {
        found = strncmp(e->d_name, prefix, strlen(prefix)) == 0;
    }
    closedir(d);
    return found;
}

void remove_with_prefix(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}
