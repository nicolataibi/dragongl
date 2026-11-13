/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * cmd_world.c - Exploration commands: look, fountain, statue, search, disarm, examine,
 * tunnel, open, close.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "server_internal.h"
#include "player.h"
#include "server_traps.h"
#include <stdarg.h>
#include <math.h>

const TempleInfo TEMPLES[] = {
    {CLASS_WIZARD, 145, 155, 101, 111, "Arcane Academy", "study"},
    {CLASS_PALADIN, 176, 186, 113, 123, "Cathedral of Justice", "celebrate"},
    {CLASS_CLERIC, 189, 199, 145, 155, "Great Temple", "pray"},
    {CLASS_SORCERER, 176, 186, 176, 186, "Spire of Blood", "pronounce"},
    {CLASS_WARLOCK, 145, 155, 189, 199, "Altar of Pacts", "invoke"},
    {CLASS_BARD, 113, 123, 176, 186, "Conservatory", "intone"},
    {CLASS_DRUID, 101, 111, 145, 155, "Sacred Grove", "awaken"},
    {CLASS_RANGER, 113, 123, 113, 123, "Hunter's Outpost", "practice"},
    {CLASS_FIGHTER, 145, 155, 67, 77, "Gladiator's Arena", "drill"},
    {CLASS_BARBARIAN, 223, 233, 145, 155, "Fighting Pit", "rage"},
    {CLASS_ROGUE, 145, 155, 223, 233, "Den of Shadows", "sneak"},
    {CLASS_MONK, 67, 77, 145, 155, "Lotus Dojo", "meditate"},
};
const int TEMPLE_COUNT = (int)(sizeof(TEMPLES) / sizeof(TEMPLES[0]));

static const char *get_voxel_name_it(VoxelType vt) {
    switch (vt) {
    case VOXEL_FLOOR:
        return "Stone floor";
    case VOXEL_COBBLE:
        return "Cobblestone street";
    case VOXEL_MARBLE:
        return "Marble floor";
    case VOXEL_WOOD:
        return "Wooden plank";
    case VOXEL_GRASS:
        return "Grassy Terrain";
    case VOXEL_SAND:
        return "Sand";
    case VOXEL_MUD:
        return "Mud";
    case VOXEL_WATER:
        return "Clear Water";
    case VOXEL_ICE:
        return "Icy Surface";
    case VOXEL_LAVA:
        return "Glowing Lava";
    case VOXEL_STAIRS_DOWN:
        return "Stairs to go down ( > )";
    case VOXEL_STAIRS_UP:
        return "Stairs to go up ( < )";
    case VOXEL_DOOR:
        return "Door";
    case VOXEL_TRAP:
        return "Hidden Trap";
    case VOXEL_ROCK:
        return "Solid Rock";
    case VOXEL_WALL:
        return "Stone Wall";
    case VOXEL_ASH:
        return "Ash";
    case VOXEL_OBSIDIAN:
        return "Obsidian";
    case VOXEL_MUSHROOM_GLOW:
        return "Bioluminescent Mushrooms";
    case VOXEL_CRYSTAL_BLUE:
    case VOXEL_CRYSTAL_PURPLE:
    case VOXEL_CRYSTAL_RED:
    case VOXEL_CRYSTAL_GREEN:
    case VOXEL_CRYSTAL_YELLOW:
    case VOXEL_CRYSTAL_ORANGE:
    case VOXEL_CRYSTAL_CYAN:
    case VOXEL_CRYSTAL_WHITE:
        return "Magic Crystal";
    default:
        return "Unknown Terrain";
    }
}

static void evaluate_digging_potential(int nx, int ny, int f, VoxelType vt, char *buf,
                                       size_t buf_size) {
    if (vt == VOXEL_GOLD_VEIN) {
        snprintf(buf, buf_size, "⚡ VERY RICH: Pure Gold Vein! Highly recommended dig!");
    } else if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
        snprintf(buf, buf_size, "✨ PRECIOUS: Raw magic crystal! Extraction recommended!");
    } else if (vt == VOXEL_WALL || vt == VOXEL_ROCK) {
        unsigned int hash = (unsigned int)(nx * 73 + ny * 37 + f * 19 + 17);
        int chance = (int)(hash % 100);
        if (chance < 15) {
            snprintf(buf, buf_size,
                     "💎 VERY PROSPEROUS: Silver and Quartz veins detected in the wall!");
        } else if (chance < 35) {
            snprintf(buf, buf_size, "🪙 PROSPEROUS: Traces of metallic minerals and pyrite.");
        } else {
            snprintf(buf, buf_size, "🪨 STERILE: Ordinary hard rock. Risk of blunting the tool.");
        }
    } else if (vt == VOXEL_MUD || vt == VOXEL_SAND || vt == VOXEL_ASH || vt == VOXEL_GRASS) {
        unsigned int hash = (unsigned int)(nx * 31 + ny * 47 + f * 13 + 5);
        int chance = (int)(hash % 100);
        if (chance < 20) {
            snprintf(buf, buf_size, "🏺 PROSPEROUS: Sediment rich in raw nuggets or finds!");
        } else {
            snprintf(buf, buf_size, "🌱 SOIL: Soft soil, easy but poor excavation.");
        }
    } else {
        snprintf(buf, buf_size, "➖ REGULAR: Non-mining surface.");
    }
}

/*Appends formatted text to a NUL-terminated buffer, truncating at cap.*/
static void buf_add(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void buf_add(char *buf, size_t cap, const char *fmt, ...) {
    size_t len = strlen(buf);
    if (len + 1 >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

static bool other_player_at(const Client *c, const Client *o, int x, int y) {
    return o->active && o->authenticated && o != c && o->floor_id == c->floor_id && o->x == x &&
           o->y == y;
}

/*What is on the player's own tile: the ground, the temple, tombstones,
 * creatures, items and other players.*/
static void look_underfoot(Client *c, Game *game, char *buf, size_t cap) {
    int px = c->x, py = c->y, f = c->floor_id;
    snprintf(buf, cap, "%s", get_voxel_name_it(game->world->floors[f].map.data[0][py][px]));
    for (int i = 0; f == 0 && i < TEMPLE_COUNT; i++) {
        const TempleInfo *t = &TEMPLES[i];
        if (px >= t->x0 && px <= t->x1 && py >= t->y0 && py <= t->y1) {
            buf_add(buf, cap, " | Temple: %s", t->name);
            break;
        }
    }
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        if (game->tombstones[i].active && game->tombstones[i].floor_id == f &&
            abs(game->tombstones[i].x - px) + abs(game->tombstones[i].y - py) <= 1) {
            buf_add(buf, cap, "| [⚰ Tombstone of %s — type 'list' to inspect]",
                    game->tombstones[i].owner);
        }
    }
    FloorIter walk = floor_iter(game, f);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        const NPC *n = &game->npcs[i];
        if (n->x != px || n->y != py) {
            continue;
        }
        if (n->archetype == ARCH_GOLD) {
            buf_add(buf, cap, "| [Golden Bag]");
        } else if (n->archetype == ARCH_TREASURE) {
            buf_add(buf, cap, "| [Treasury Chest]");
        } else if (n->is_ghost) {
            buf_add(buf, cap, "| [%s remains]", n->custom_name);
        } else if (n->archetype == ARCH_MERCHANT) {
            buf_add(buf, cap, " | [%s]", n->merchant.shop_name);
        } else if (n->template) {
            buf_add(buf, cap, " | [%s HP:%d/%d]", n->template->name, n->hp, n->max_hp);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (other_player_at(c, &game->clients[i], px, py)) {
            buf_add(buf, cap, "| [Player: %s]", game->clients[i].username);
        }
    }
}

/*The eight sectors of the look table: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW,
 * 6=W, 7=NW.*/
typedef struct {
    char text[8][256];
    int count[8];
} LookSectors;

/*Sector of the offset (dx, dy); a negative dy is north.*/
static int look_sector(int dx, int dy) {
    float deg = atan2f(-(float)dy, (float)dx) * (180.0f / 3.1415926535f);
    if (deg >= 157.5f || deg < -157.5f) {
        return 6;
    }
    /*-157.5 .. 157.5 in steps of 45 degrees: SW, S, SE, E, NE, N, NW*/
    static const int k_by_step[7] = {5, 4, 3, 2, 1, 0, 7};
    int step = (int)floorf((deg + 157.5f) / 45.0f);
    if (step > 6) {
        step = 6;
    }
    return k_by_step[step < 0 ? 0 : step];
}

static void sector_add(LookSectors *s, int sector, const char *what, int steps) {
    buf_add(s->text[sector], sizeof(s->text[sector]), "%s%s (%dp)",
            s->count[sector] > 0 ? ", " : "", what, steps);
    s->count[sector]++;
}

/*How a creature or an item shows in the look table.*/
static void look_npc_label(const NPC *n, char *out, size_t cap) {
    if (n->archetype == ARCH_MERCHANT) {
        snprintf(out, cap, "%s", n->merchant.shop_name);
    } else if (n->archetype == ARCH_GOLD) {
        snprintf(out, cap, "Gold");
    } else if (n->archetype == ARCH_TREASURE) {
        snprintf(out, cap, "Treasure");
    } else if (n->is_ghost) {
        snprintf(out, cap, "Remains-%s", n->custom_name);
    } else {
        snprintf(out, cap, "%s", n->template ? n->template->name : "Entity");
    }
}

/*The notable feature of a voxel, or NULL.*/
static const char *look_voxel_label(VoxelType vt, int floor_id) {
    if (vt == VOXEL_STAIRS_DOWN) {
        return "Stairs-Down";
    }
    if (vt == VOXEL_STAIRS_UP) {
        return "Stairs-Up";
    }
    if (floor_id == 0 && (vt == VOXEL_WATER || vt == VOXEL_ICE)) {
        return "Sacred-Fountain";
    }
    if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
        return "Crystal";
    }
    return vt == VOXEL_TRAP ? "Trap" : NULL;
}

/*Everything worth a mention on the cell (nx, ny), `steps` steps away.*/
static void look_cell(Client *c, Game *game, LookSectors *s, int sector, int nx, int ny,
                      int steps) {
    int f = c->floor_id;
    char label[96];
    FloorIter walk = floor_iter(game, f);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].x == nx && game->npcs[i].y == ny) {
            look_npc_label(&game->npcs[i], label, sizeof(label));
            sector_add(s, sector, label, steps);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (other_player_at(c, &game->clients[i], nx, ny)) {
            snprintf(label, sizeof(label), "Player-%s", game->clients[i].username);
            sector_add(s, sector, label, steps);
        }
    }
    const char *feature = look_voxel_label(game->world->floors[f].map.data[0][ny][nx], f);
    if (feature) {
        sector_add(s, sector, feature, steps);
    }
}

bool cmd_look(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    int vis_rad = get_vision_radius(c);
    if (vis_rad <= 0) {
        send_text_to_client(
            c->sock,
            "[INSPECTION] You can't see anything: you are surrounded by darkness or blinded!");
        return true;
    }
    char feet_buf[512];
    look_underfoot(c, game, feet_buf, sizeof(feet_buf));

    static const char *const sector_names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    LookSectors sectors;
    memset(&sectors, 0, sizeof(sectors));
    for (int dy = -vis_rad; dy <= vis_rad; dy++) {
        for (int dx = -vis_rad; dx <= vis_rad; dx++) {
            int nx = c->x + dx;
            int ny = c->y + dy;
            float dist = sqrtf((float)(dx * dx + dy * dy));
            if ((dx == 0 && dy == 0) || nx < 0 || nx >= MAP_WIDTH || ny < 0 || ny >= MAP_HEIGHT ||
                dist > (float)vis_rad) {
                continue;
            }
            int sector = look_sector(dx, dy);
            if (strlen(sectors.text[sector]) <= 180) { /*a full row takes no more*/
                look_cell(c, game, &sectors, sector, nx, ny, (int)roundf(dist));
            }
        }
    }

    static const char *const rule =
        "+--------+-------------------------------------------------------------+";
    send_text_to_client(c->sock, "[INSPECTION] Illuminated View Range: %d Steps (%d Floor)",
                        vis_rad, c->floor_id);
    send_text_to_client(c->sock, "[UNDER FEET] Position (%d, %d): %s", c->x, c->y, feet_buf);
    send_text_to_client(c->sock, "%s", rule);
    send_text_to_client(c->sock, "| %-6s | %-59s |", "SECTOR",
                        "DETECTIONS IN THE ILLUMINATED AREA");
    send_text_to_client(c->sock, "%s", rule);
    for (int s = 0; s < 8; s++) {
        send_text_to_client(c->sock, "| %-6s | %-59.59s |", sector_names[s],
                            sectors.count[s] == 0 ? "Free" : sectors.text[s]);
    }
    send_text_to_client(c->sock, "%s", rule);
    return true;
}

bool cmd_fountain(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    if (c->floor_id == 0) {
        bool near_fountain = false;
        for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                int nx = c->x + dx;
                int ny = c->y + dy;
                if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                    VoxelType vt = game->world->floors[0].map.data[0][ny][nx];
                    if (vt == VOXEL_WATER || vt == VOXEL_ICE) {
                        near_fountain = true;
                    }
                }
            }
        }
        if (near_fountain) {
            c->hp = c->max_hp;
            send_detailed_state(game, c); //Sync HP with the HUD client
            send_text_to_client(
                c->sock,
                "[SYSTEM] Drink the water from the sacred fountain and feel your wounds magically heal!");
        } else {
            send_text_to_client(c->sock, "[ERROR] You are not near any healing fountains.");
        }
    } else {
        send_text_to_client(c->sock,
                            "[ERROR] Healing fountains are only found in the Magic Sanctuary.");
    }
    return true;
}

bool cmd_statue(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    if (c->floor_id == 0) {
        bool near_crystal = false;
        for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                int nx = c->x + dx;
                int ny = c->y + dy;
                if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                    VoxelType vt = game->world->floors[0].map.data[0][ny][nx];
                    if ((vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_PURPLE) ||
                        (vt >= VOXEL_CRYSTAL_RED && vt <= VOXEL_CRYSTAL_WHITE)) {
                        near_crystal = true;
                    }
                }
            }
        }
        if (near_crystal) {
            for (int sl = 1; sl <= 9; sl++) {
                c->spell_slots[sl] = c->spell_slots_max[sl];
            }
            c->hunger_level = 0;
            c->exhaustion_level = 0;
            send_detailed_state(game,
                                c); //Sync spell slots, hunger and exhaustion with the HUD client
            send_text_to_client(
                c->sock,
                "[SYSTEM] Meditate at the foot of the crystal monument. Your spirit is renewed, chasing away fatigue, hunger and recharging your spells!");
        } else {
            send_text_to_client(c->sock, "[ERROR] You are not near any crystal statues.");
        }
    } else {
        send_text_to_client(c->sock,
                            "[ERROR] Restoration statues are only found in the Magic Sanctuary.");
    }
    return true;
}

bool cmd_search(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    int roll = (rand() % 20) + 1;
    int perception = roll + rules_get_modifier(c->wis);
    bool found_something = false;

    Floor *f = &game->world->floors[c->floor_id];
    for (int i = 0; i < f->trap_count; i++) {
        Trap *t = &f->traps[i];
        if (!t->active || t->detected) {
            continue;
        }

        int check_x = t->x, check_y = t->y;
        if (t->type == 1) { // TRAP_DART_WALL
            int wall_dx[4] = {0, 1, 0, -1};
            int wall_dy[4] = {-1, 0, 1, 0};
            check_x += wall_dx[t->wall_dir];
            check_y += wall_dy[t->wall_dir];
        }

        if (abs(c->x - check_x) <= 2 && abs(c->y - check_y) <= 2) {
            if (perception >= t->detection_dc) {
                t->detected = true;
                found_something = true;
                send_text_to_client(
                    c->sock, "[PERCEPTION] You have discovered a hidden trap! (Roll: %d vs DC: %d)",
                    perception, t->detection_dc);
            }
        }
    }

    if (!found_something) {
        send_text_to_client(
            c->sock,
            "[SYSTEM] You examine the area carefully, but you don't notice anything unusual.");
    }
    return true;
}

/*The trap to work on: the nearest active one within 2 steps (a wall trap
 * counts from the tile it fires onto); a detected trap wins over a hidden
 * one that is not closer.*/
static Trap *disarm_target(const Client *c, Floor *f) {
    static const int wall_dx[4] = {0, 1, 0, -1};
    static const int wall_dy[4] = {-1, 0, 1, 0};
    Trap *target = NULL;
    int min_dist = 999;
    for (int i = 0; i < f->trap_count; i++) {
        Trap *t = &f->traps[i];
        if (!t->active) {
            continue;
        }
        int tx = t->x, ty = t->y;
        if (t->type == TRAP_DART_WALL || t->type == TRAP_SPRING_SPEAR) {
            tx += wall_dx[t->wall_dir];
            ty += wall_dy[t->wall_dir];
        }
        int dist = abs(c->x - tx) + abs(c->y - ty);
        if (dist <= 2 && dist < min_dist && (t->detected || !target)) {
            min_dist = dist;
            target = t;
        }
    }
    return target;
}

static bool trap_is_magical(TrapType type) {
    return type == TRAP_CURSE_RUNE || type == TRAP_ANTIMAGIC_ZONE || type == TRAP_ILLUSION_MIRROR ||
           type == TRAP_INVERSION_RUNE || type == TRAP_DARKNESS || type == TRAP_SILENCE ||
           type == TRAP_TELEPORT || type == TRAP_CRYSTAL_BURST;
}

static bool has_thief_tools(const Client *c) {
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == -1) {
            continue;
        }
        const char *iname = item_database[c->backpack[i].template_idx].name;
        if (strcasestr(iname, "Tool") || strcasestr(iname, "Lockpick") ||
            strcasestr(iname, "Kit")) {
            return true;
        }
    }
    return false;
}

/*Modifier of the disarm check. Arcane: the better of INT and WIS, +4 for
 * a level-1 slot (spent here). Manual: DEX, plus the proficiency bonus
 * with tools, twice for a rogue.*/
static int disarm_modifier(Client *c, bool arcane) {
    if (arcane) {
        int mod = rules_get_modifier((c->intel > c->wis) ? c->intel : c->wis);
        if (c->spell_slots[1] > 0) {
            c->spell_slots[1]--;
            mod += 4;
            send_text_to_client(c->sock,
                                "[MAGIC] You channel your arcane energy (-1 Slot Lv.1, Focus +4)!");
        }
        return mod;
    }
    int mod = rules_get_modifier(c->dex);
    int prof_bonus = 2 + (c->level - 1) / 4;
    if (c->class_id == CLASS_ROGUE) {
        mod += prof_bonus * 2;
    } else if (has_thief_tools(c)) {
        mod += prof_bonus;
    }
    return mod;
}

/*The disarm check of a player: the die, the modifier, the DC.*/
typedef struct {
    const char *method;
    int roll, mod, total, dc;
    bool salvage;
} DisarmCheck;

static void disarm_success(Client *c, Floor *f, Trap *t, const DisarmCheck *k) {
    t->active = false;
    t->detected = false;
    t->respawn_timer = RESPAWN_TRAPS_TICKS;
    if (f->map.data[0][t->y][t->x] == VOXEL_TRAP) {
        f->map.data[0][t->y][t->x] = VOXEL_FLOOR;
    }
    int xp_reward = 25 + c->floor_id * 5;
    if (k->roll == 20) {
        xp_reward *= 2;
        send_text_to_client(
            c->sock, "[DISARMS] CRITICAL SUCCESS! [%s] (Roll: 20 Nat + Mod: %d = %d vs DC: %d)",
            k->method, k->mod, k->total, k->dc);
        send_text_to_client(
            c->sock, "[DISARMS] With extraordinary skill you dismantle the trap safely (+%d XP)!",
            xp_reward);
    } else {
        send_text_to_client(c->sock, "[DISARMS] SUCCESS! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)",
                            k->method, k->roll, k->mod, k->total, k->dc);
        send_text_to_client(c->sock, "[DISARMS] You successfully neutralize the trap (+%d XP).",
                            xp_reward);
    }
    c->xp += xp_reward;
    if (k->salvage) {
        int gold_salvage = 40 + c->floor_id * 15 + (rand() % 30);
        c->gold += (uint64_t)gold_salvage;
        send_text_to_client(
            c->sock,
            "[RECOVERY] Extract valuable materials and alchemical resources from the mechanism (+%lu Gold Coins)!",
            (unsigned long)gold_salvage);
    }
}

/*A fumble: the trap fires on the player as if he had stepped on it.*/
static void disarm_backfire(Client *c, Trap *t, const DisarmCheck *k, Game *game) {
    send_text_to_client(c->sock,
                        "[DISARMS] CATASTROPHIC FAILURE! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)",
                        k->method, k->roll, k->mod, k->total, k->dc);
    send_text_to_client(c->sock, "[DANGER] An execution error triggers the trap!");
    bool has_shield = ((c->slot_hand_r.template_idx != -1 &&
                        item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD) ||
                       (c->slot_hand_l.template_idx != -1 &&
                        item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD));
    if (has_shield && !trap_is_magical(t->type)) {
        send_text_to_client(
            c->sock,
            "[DEFENSE] You instinctively raise your shield to protect yourself from the trap's blow!");
    }
    int orig_x = c->x, orig_y = c->y;
    c->x = t->x;
    c->y = t->y;
    check_traps(c, game);
    c->x = orig_x;
    c->y = orig_y;
}

/*disarm [salvage|r] [magic|m]: a d20 + modifier against the trap's DC
 * (+3 to salvage its materials). A natural 20 always succeeds; a natural
 * 1, or missing by 5 or more, fires the trap.*/
bool cmd_disarm(Client *c, const char *cmd, Game *game) {
    Floor *f = &game->world->floors[c->floor_id];
    Trap *t = disarm_target(c, f);
    if (!t) {
        send_text_to_client(c->sock,
                            "[ERROR] No traps detected in range. Use 'search' to find one.");
        return true;
    }
    t->detected = true;

    bool arcane =
        strstr(cmd, "magic") != NULL || strstr(cmd, " m") != NULL || trap_is_magical(t->type);
    DisarmCheck k;
    k.salvage = strstr(cmd, "salvage") != NULL || strstr(cmd, " r") != NULL;
    k.method = arcane ? "Arcane Dissipation" : "Manual Dexterity";
    k.roll = (rand() % 20) + 1;
    k.dc = (t->detection_dc > 0 ? t->detection_dc : 12) + (k.salvage ? 3 : 0);
    k.mod = disarm_modifier(c, arcane);
    k.total = k.roll + k.mod;

    if (k.roll == 20 || k.total >= k.dc) {
        disarm_success(c, f, t, &k);
    } else if (k.roll == 1 || k.total <= k.dc - 5) {
        disarm_backfire(c, t, &k, game);
    } else {
        send_text_to_client(c->sock, "[DISARMS] FAILURE! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)",
                            k.method, k.roll, k.mod, k.total, k.dc);
        send_text_to_client(
            c->sock,
            "[DISARMS] The attempt fails, but you retreat in time before triggering the mechanism.");
    }
    return true;
}

bool cmd_examine(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    send_text_to_client(c->sock, "[DETAILED EXAMINATION] Area 1 step (Position %d, %d - Floor %d)",
                        c->x, c->y, c->floor_id);
    send_text_to_client(
        c->sock,
        "+--------+-----------------------+-------------------------------------------------------+");
    send_text_to_client(c->sock,
                        "| SQUARE. | TYPE GROUND / WALL | MINING EVALUATION AND ATTENDANCE |");
    send_text_to_client(
        c->sock,
        "+--------+-----------------------+-------------------------------------------------------+");

    int qx[] = {0, 0, 1, 1, 1, 0, -1, -1, -1};
    int qy[] = {0, -1, -1, 0, 1, 1, 1, 0, -1};
    const char *qnames[] = {"FEET", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};

    Floor *f = &game->world->floors[c->floor_id];

    for (int i = 0; i < 9; i++) {
        int nx = c->x + qx[i];
        int ny = c->y + qy[i];
        if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
            VoxelType vt = f->map.data[0][ny][nx];
            const char *vname = get_voxel_name_it(vt);

            char v_min[128];
            evaluate_digging_potential(nx, ny, c->floor_id, vt, v_min, sizeof(v_min));

            char presence[256] = "";

            FloorIter walk = floor_iter(game, c->floor_id);
            for (int m; (m = floor_iter_next(&walk)) >= 0;) {
                if (game->npcs[m].x == nx && game->npcs[m].y == ny) {
                    if (game->npcs[m].archetype == ARCH_GOLD) {
                        strncat(presence, "| [Golden Bag]",
                                sizeof(presence) - strlen(presence) - 1);
                    } else if (game->npcs[m].archetype == ARCH_TREASURE) {
                        strncat(presence, "| [Treasury Chest]",
                                sizeof(presence) - strlen(presence) - 1);
                    } else if (game->npcs[m].is_ghost) {
                        char gbuf[96]; /*room for a 63-char name + decoration*/
                        snprintf(gbuf, sizeof(gbuf), "| [%s remains]", game->npcs[m].custom_name);
                        strncat(presence, gbuf, sizeof(presence) - strlen(presence) - 1);
                    } else if (game->npcs[m].archetype == ARCH_MERCHANT) {
                        char mbuf[96];
                        snprintf(mbuf, sizeof(mbuf), " | [%s]", game->npcs[m].merchant.shop_name);
                        strncat(presence, mbuf, sizeof(presence) - strlen(presence) - 1);
                    } else if (game->npcs[m].template) {
                        char nbuf[128];
                        snprintf(nbuf, sizeof(nbuf), " | %s (HP:%d/%d)",
                                 game->npcs[m].template->name, game->npcs[m].hp,
                                 game->npcs[m].max_hp);
                        strncat(presence, nbuf, sizeof(presence) - strlen(presence) - 1);
                    }
                }
            }

            char combined[512];
            snprintf(combined, sizeof(combined), "%s%s", v_min, presence);

            send_text_to_client(c->sock, "| %-6s | %-21s | %-53s |", qnames[i], vname, combined);
        }
    }

    send_text_to_client(
        c->sock,
        "+--------+-----------------------+-------------------------------------------------------+");

    bool has_mining_tool = false;
    char tool_name[64] = "";
    ItemInstance *hands[] = {&c->slot_hand_r, &c->slot_hand_l};
    for (int i = 0; i < 2; i++) {
        if (hands[i]->template_idx != -1) {
            const char *iname = item_database[hands[i]->template_idx].name;
            if (strcasestr(iname, "Pick") || strcasestr(iname, "Shovel") ||
                strcasestr(iname, "Hammer") || strcasestr(iname, "Mining") ||
                strcasestr(iname, "Tool")) {
                has_mining_tool = true;
                snprintf(tool_name, sizeof(tool_name), "%s", iname);
                break;
            }
        }
    }
    if (!has_mining_tool) {
        for (int i = 0; i < c->backpack_count; i++) {
            if (c->backpack[i].template_idx != -1) {
                const char *iname = item_database[c->backpack[i].template_idx].name;
                if (strcasestr(iname, "Pick") || strcasestr(iname, "Shovel") ||
                    strcasestr(iname, "Hammer") || strcasestr(iname, "Mining") ||
                    strcasestr(iname, "Tool")) {
                    has_mining_tool = true;
                    snprintf(tool_name, sizeof(tool_name), "%s", iname);
                    break;
                }
            }
        }
    }

    if (has_mining_tool) {
        send_text_to_client(
            c->sock, "[TOOLS] You have the right tool (%s). Use 'tunnel <dir>' to dig!", tool_name);
    } else {
        send_text_to_client(
            c->sock,
            "[TOOLS] No pickaxes or shovels. Get a tool for extracting minerals and treasures.");
    }

    return true;
}

/*--- Door memory ---
 * 'close' may ONLY turn back a tile that this server turned from a door
 * into floor with 'open': closing any floor tile would let a player seal
 * corridors for good (the map is saved). The registry is per floor,
 * bounded, and volatile (after a restart an opened door stays open).*/
static struct {
    int x, y;
} g_recently_opened_doors[MAX_FLOORS][32];
static int g_recently_opened_door_n[MAX_FLOORS] = {0};

bool cmd_open(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
        send_text_to_client(c->sock, "[SYSTEM] Use: open <n|s|e|w>");
        return true;
    }
    dir_str++;
    int tx = c->x;
    int ty = c->y;
    if (*dir_str == 'n') {
        ty--;
    } else if (*dir_str == 's') {
        ty++;
    } else if (*dir_str == 'e') {
        tx++;
    } else if (*dir_str == 'w') {
        tx--;
    } else {
        send_text_to_client(c->sock, "[SYSTEM] Use: open <n|s|e|w>");
        return true;
    }
    /*Bounds: at the map edge tx/ty can step outside the array.*/
    /*The outermost ring of rock is the edge of the world: it is never dug.*/
    if (tx < 1 || tx >= MAP_WIDTH - 1 || ty < 1 || ty >= MAP_HEIGHT - 1) {
        send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
        return true;
    }
    Floor *f = &game->world->floors[c->floor_id];
    if (f->map.data[0][ty][tx] == VOXEL_DOOR) {
        f->map.data[0][ty][tx] = VOXEL_FLOOR;
        /*Remember it so 'close' can put the door back.*/
        bool known = false;
        for (int i = 0; i < g_recently_opened_door_n[c->floor_id]; i++) {
            if (g_recently_opened_doors[c->floor_id][i].x == tx &&
                g_recently_opened_doors[c->floor_id][i].y == ty) {
                known = true;
                break;
            }
        }
        if (!known && g_recently_opened_door_n[c->floor_id] < 32) {
            g_recently_opened_doors[c->floor_id][g_recently_opened_door_n[c->floor_id]].x = tx;
            g_recently_opened_doors[c->floor_id][g_recently_opened_door_n[c->floor_id]].y = ty;
            g_recently_opened_door_n[c->floor_id]++;
        }
        send_text_to_client(c->sock, "[SYSTEM] You opened the door.");
    } else {
        send_text_to_client(c->sock, "[SYSTEM] There is no door there.");
    }
    return true;
}

bool cmd_close(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
        send_text_to_client(c->sock, "[SYSTEM] Use: close <n|s|e|w>");
        return true;
    }
    dir_str++;
    int tx = c->x;
    int ty = c->y;
    if (*dir_str == 'n') {
        ty--;
    } else if (*dir_str == 's') {
        ty++;
    } else if (*dir_str == 'e') {
        tx++;
    } else if (*dir_str == 'w') {
        tx--;
    } else {
        send_text_to_client(c->sock, "[SYSTEM] Use: close <n|s|e|w>");
        return true;
    }
    /*The outermost ring of rock is the edge of the world: it is never dug.*/
    if (tx < 1 || tx >= MAP_WIDTH - 1 || ty < 1 || ty >= MAP_HEIGHT - 1) {
        send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
        return true;
    }
    Floor *f = &game->world->floors[c->floor_id];
    /*Only a floor tile present in the "recently opened doors" registry
   * can become a door again.*/
    bool was_door = false;
    for (int i = 0; i < g_recently_opened_door_n[c->floor_id]; i++) {
        if (g_recently_opened_doors[c->floor_id][i].x == tx &&
            g_recently_opened_doors[c->floor_id][i].y == ty) {
            /*Remove it from the registry.*/
            g_recently_opened_doors[c->floor_id][i] =
                g_recently_opened_doors[c->floor_id][g_recently_opened_door_n[c->floor_id] - 1];
            g_recently_opened_door_n[c->floor_id]--;
            was_door = true;
            break;
        }
    }
    if (f->map.data[0][ty][tx] == VOXEL_FLOOR && was_door) {
        f->map.data[0][ty][tx] = VOXEL_DOOR;
        send_text_to_client(c->sock, "[SYSTEM] You closed the door.");
    } else {
        send_text_to_client(c->sock, "[SYSTEM] There is no open door to close there.");
    }
    return true;
}
