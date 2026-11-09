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

#include "persist.h"

#include "data_loader.h"

#include "writer.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRYSTAL_RESPAWN_MAX 100
#define GHOST_LOOT_MAX      30

static char g_state_dir[STATE_DIR_MAX];

/* --- State directory ---------------------------------------------------------- */

/* mkdir -p with mode 0700 for every directory it creates. */
static bool make_dirs(const char *path, char *err, size_t err_len) {
    char buf[STATE_DIR_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        snprintf(err, err_len, "state directory path too long");
        return false;
    }
    for (char *p = buf + 1;; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
                snprintf(err, err_len, "cannot create %s: %s", buf, strerror(errno));
                return false;
            }
            *p = saved;
            if (saved == '\0') {
                break;
            }
        }
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_len, "%s is not a directory", path);
        return false;
    }
    if (access(path, W_OK | X_OK) != 0) {
        snprintf(err, err_len, "%s is not writable: %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool persist_init_state_dir(const char *explicit_dir, char *err, size_t err_len) {
    char dir[STATE_DIR_MAX];
    const char *env = getenv("DRAGONGL_STATE_DIR");
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int n;
    if (explicit_dir && explicit_dir[0]) {
        n = snprintf(dir, sizeof(dir), "%s", explicit_dir);
    } else if (env && env[0]) {
        n = snprintf(dir, sizeof(dir), "%s", env);
    } else if (xdg && xdg[0] == '/') {
        n = snprintf(dir, sizeof(dir), "%s/dragongl", xdg);
    } else if (home && home[0] == '/') {
        n = snprintf(dir, sizeof(dir), "%s/.local/state/dragongl", home);
    } else {
        snprintf(err, err_len, "no state directory: use --state-dir or set DRAGONGL_STATE_DIR");
        return false;
    }
    if (n < 0 || n >= (int)sizeof(dir) - 32) {
        snprintf(err, err_len, "state directory path too long");
        return false;
    }
    /* Drop trailing slashes so the joined paths stay clean. */
    while (n > 1 && dir[n - 1] == '/') {
        dir[--n] = '\0';
    }
    char sub[STATE_DIR_MAX + 32]; /* dir is at most STATE_DIR_MAX - 32 */
    if (!make_dirs(dir, err, err_len)) {
        return false;
    }
    snprintf(sub, sizeof(sub), "%s/saves", dir);
    if (!make_dirs(sub, err, err_len)) {
        return false;
    }
    snprintf(sub, sizeof(sub), "%s/maps", dir);
    if (!make_dirs(sub, err, err_len)) {
        return false;
    }
    /*One server per state directory: two would overwrite each other's
     * world and saves. The lock lives as long as the process.*/
    snprintf(sub, sizeof(sub), "%s/server.lock", dir);
    int lock_fd = open(sub, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s", sub, strerror(errno));
        return false;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        snprintf(err, err_len, "%s is in use by another dragongl_server", dir);
        return false;
    }
    memcpy(g_state_dir, dir, (size_t)n + 1);
    return true;
}

const char *persist_state_dir(void) {
    return g_state_dir;
}

bool state_path(char *out, size_t out_len, const char *fmt, ...) {
    char rel[STATE_DIR_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rel, sizeof(rel), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(rel) ||
        snprintf(out, out_len, "%s/%s", g_state_dir, rel) >= (int)out_len) {
        if (out_len) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

static void cleanup_tmp_in(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".tmp") == 0) {
            char p[STATE_DIR_MAX + 300];
            if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) < (int)sizeof(p)) {
                unlink(p);
            }
        }
    }
    closedir(d);
}

void persist_cleanup_tmp(void) {
    char p[STATE_DIR_MAX + 16];
    cleanup_tmp_in(g_state_dir);
    if (state_path(p, sizeof(p), "saves")) {
        cleanup_tmp_in(p);
    }
}

/* --- Shared records ------------------------------------------------------------- */

static void put_item(BinWriter *w, const ItemInstance *it) {
    bw_i32(w, it->template_idx);
    bw_i32(w, it->durability);
    bw_i32(w, it->stack_count);
    bw_bool(w, it->is_identified);
    bw_i32(w, it->current_charges);
    bw_i32(w, it->to_hit_bonus);
    bw_i32(w, it->to_dam_bonus);
    bw_i32(w, it->ac_bonus);
    bw_i32(w, (int32_t)it->quality);
    bw_i32(w, (int32_t)it->blessing);
    bw_i32(w, (int32_t)it->element);
    bw_bool(w, it->is_artifact);
    bw_str(w, it->artifact_name, sizeof(it->artifact_name));
    bw_i32(w, it->artifact_str_bonus);
    bw_i32(w, it->artifact_dex_bonus);
    bw_i32(w, it->artifact_con_bonus);
}

/*An enum value in [lo, hi]; anything else fails the read (and yields lo,
 * so no out-of-range value is ever stored in an enum).*/
static int32_t get_enum(BinReader *r, int32_t lo, int32_t hi) {
    int32_t v = br_i32(r);
    if (v < lo || v > hi) {
        r->failed = true;
        return lo;
    }
    return v;
}

static void get_item(BinReader *r, ItemInstance *it) {
    memset(it, 0, sizeof(*it));
    it->template_idx = br_i32(r);
    it->durability = br_i32(r);
    it->stack_count = br_i32(r);
    it->is_identified = br_bool(r);
    it->current_charges = br_i32(r);
    it->to_hit_bonus = br_i32(r);
    it->to_dam_bonus = br_i32(r);
    it->ac_bonus = br_i32(r);
    it->quality = (ItemQuality)get_enum(r, QUALITY_RUSTY, QUALITY_MASTERWORK);
    it->blessing = (ItemBlessing)get_enum(r, BLESS_CURSED, BLESS_BLESSED);
    it->element = (ItemElement)get_enum(r, ELEM_NONE, ELEM_POISONOUS);
    it->is_artifact = br_bool(r);
    br_str(r, it->artifact_name, sizeof(it->artifact_name));
    it->artifact_str_bonus = br_i32(r);
    it->artifact_dex_bonus = br_i32(r);
    it->artifact_con_bonus = br_i32(r);
}

static void put_items(BinWriter *w, const ItemInstance *items, int n) {
    bw_u32(w, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        put_item(w, &items[i]);
    }
}

/* Arrays are stored with their length; the reader requires exactly `n`. */
static void get_items(BinReader *r, ItemInstance *items, int n) {
    if (br_u32(r) != (uint32_t)n) {
        r->failed = true;
        return;
    }
    for (int i = 0; i < n && !r->failed; i++) {
        get_item(r, &items[i]);
    }
}

/* Sparse form for mostly empty arrays (an NPC's ghost loot): only the
 * slots that are not all-zero bytes are stored, each with its index, in
 * increasing order. The others are restored as all-zero, exactly as they
 * were. Keeps npcs.dat at a few MB instead of ~120 MB. */
static bool item_is_zero(const ItemInstance *it) {
    static const ItemInstance zero;
    return item_instance_equal(it, &zero);
}

static void put_sparse_items(BinWriter *w, const ItemInstance *items, int n) {
    uint32_t used = 0;
    for (int i = 0; i < n; i++) {
        if (!item_is_zero(&items[i])) {
            used++;
        }
    }
    bw_u32(w, (uint32_t)n);
    bw_u32(w, used);
    for (int i = 0; i < n; i++) {
        if (!item_is_zero(&items[i])) {
            bw_u32(w, (uint32_t)i);
            put_item(w, &items[i]);
        }
    }
}

static void get_sparse_items(BinReader *r, ItemInstance *items, int n) {
    memset(items, 0, sizeof(ItemInstance) * (size_t)n);
    if (br_u32(r) != (uint32_t)n) {
        r->failed = true;
        return;
    }
    uint32_t used = br_count(r, (uint32_t)n);
    int64_t last = -1;
    for (uint32_t k = 0; k < used && !r->failed; k++) {
        uint32_t idx = br_u32(r);
        if ((int64_t)idx <= last || idx >= (uint32_t)n) {
            r->failed = true;
            return;
        }
        last = idx;
        get_item(r, &items[idx]);
    }
}

static void put_i32s(BinWriter *w, const int *v, int n) {
    bw_u32(w, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        bw_i32(w, v[i]);
    }
}

static void get_i32s(BinReader *r, int *v, int n) {
    if (br_u32(r) != (uint32_t)n) {
        r->failed = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        v[i] = br_i32(r);
    }
}

static BinFileStatus read_payload(const char *path, uint32_t magic, uint16_t version, BinReader *r,
                                  uint8_t **buf, char *why, size_t why_len) {
    size_t len = 0;
    BinFileStatus st = binfile_read(path, magic, version, buf, &len, why, why_len);
    if (st == BINFILE_OK) {
        br_init(r, *buf, len);
    }
    return st;
}

static BinFileStatus finish_read(BinReader *r, uint8_t *buf, char *why, size_t why_len) {
    bool ok = br_done(r);
    free(buf);
    if (!ok) {
        if (why && why_len) {
            snprintf(why, why_len, "malformed content");
        }
        return BINFILE_INVALID;
    }
    return BINFILE_OK;
}

/* The snapshot is encoded by the calling thread; the disk part goes to
 * the writer (writer.h), which takes ownership of the buffer. */
static bool write_and_free(const char *path, uint32_t magic, uint16_t version, BinWriter *w) {
    return writer_submit(path, magic, version, w);
}

/* --- Player save --------------------------------------------------------------- */

bool persist_write_player(const char *path, const SaveData *sd) {
    BinWriter w;
    bw_init(&w);
    bw_str(&w, sd->password_hash, sizeof(sd->password_hash));
    bw_i32(&w, sd->x);
    bw_i32(&w, sd->y);
    bw_i32(&w, sd->floor_id);
    bw_i32(&w, (int32_t)sd->race_id);
    bw_i32(&w, (int32_t)sd->subrace_id);
    bw_i32(&w, (int32_t)sd->class_id);
    bw_i32(&w, sd->level);
    bw_i32(&w, sd->xp);
    bw_i32(&w, sd->str);
    bw_i32(&w, sd->dex);
    bw_i32(&w, sd->con);
    bw_i32(&w, sd->intel);
    bw_i32(&w, sd->wis);
    bw_i32(&w, sd->cha);
    bw_u64(&w, sd->gold);
    bw_i32(&w, sd->hp);
    bw_i32(&w, sd->max_hp);
    bw_i32(&w, sd->backpack_count);
    put_items(&w, sd->backpack, MAX_BACKPACK);
    put_items(&w, sd->belt, MAX_BELT);
    const ItemInstance *slots[] = {&sd->s_head,   &sd->s_neck,   &sd->s_body,  &sd->s_back,
                                   &sd->s_hand_r, &sd->s_hand_l, &sd->s_hands, &sd->s_arm_r,
                                   &sd->s_arm_l,  &sd->s_feet};
    bw_u32(&w, (uint32_t)(sizeof(slots) / sizeof(slots[0])));
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        put_item(&w, slots[i]);
    }
    put_items(&w, sd->s_rings, 10);
    put_i32s(&w, sd->spell_slots, MAX_SPELL_LEVEL + 1);
    put_i32s(&w, sd->spell_slots_max, MAX_SPELL_LEVEL + 1);
    bw_i32(&w, sd->alignment);
    bw_u32(&w, (uint32_t)sd->effect_count);
    for (int i = 0; i < sd->effect_count && i < MAX_EFFECTS_PER_ENTITY; i++) {
        const SavedEffect *e = &sd->effects[i];
        bw_i32(&w, e->cond);
        bw_i32(&w, e->trigger);
        bw_i32(&w, e->mod_type);
        bw_i32(&w, e->value);
        bw_i32(&w, e->duration_rounds);
        bw_bool(&w, e->is_persistent != 0);
    }
    bw_i32(&w, sd->light_turns_left);
    bw_i32(&w, sd->hunger_level);
    bw_i32(&w, sd->exhaustion_level);
    bw_i32(&w, sd->total_kills);
    bw_i32(&w, sd->total_steps);
    bw_i32(&w, sd->max_floor_explored);
    bw_u32(&w, KNOWN_SPELLS_WORDS);
    for (int i = 0; i < KNOWN_SPELLS_WORDS; i++) {
        bw_u64(&w, sd->known_spells[i]);
    }
    bw_u32(&w, sd->bosses_defeated);
    bw_i32(&w, sd->unspent_stat_points);
    bw_u32(&w, (uint32_t)sd->blocked_count);
    for (int i = 0; i < sd->blocked_count && i < MAX_BLOCKED_PLAYERS; i++) {
        bw_str(&w, sd->blocked_players[i], sizeof(sd->blocked_players[i]));
    }
    return write_and_free(path, PERSIST_SAVE_MAGIC, PERSIST_SAVE_VERSION, &w);
}

BinFileStatus persist_read_player(const char *path, SaveData *sd, char *why, size_t why_len) {
    BinReader r;
    uint8_t *buf = NULL;
    BinFileStatus st =
        read_payload(path, PERSIST_SAVE_MAGIC, PERSIST_SAVE_VERSION, &r, &buf, why, why_len);
    if (st != BINFILE_OK) {
        return st;
    }
    memset(sd, 0, sizeof(*sd));
    br_str(&r, sd->password_hash, sizeof(sd->password_hash));
    sd->x = br_i32(&r);
    sd->y = br_i32(&r);
    sd->floor_id = br_i32(&r);
    sd->race_id = (RaceType)br_i32(&r);
    sd->subrace_id = (SubraceType)br_i32(&r);
    sd->class_id = (ClassType)br_i32(&r);
    sd->level = br_i32(&r);
    sd->xp = br_i32(&r);
    sd->str = br_i32(&r);
    sd->dex = br_i32(&r);
    sd->con = br_i32(&r);
    sd->intel = br_i32(&r);
    sd->wis = br_i32(&r);
    sd->cha = br_i32(&r);
    sd->gold = br_u64(&r);
    sd->hp = br_i32(&r);
    sd->max_hp = br_i32(&r);
    sd->backpack_count = br_i32(&r);
    get_items(&r, sd->backpack, MAX_BACKPACK);
    get_items(&r, sd->belt, MAX_BELT);
    ItemInstance *slots[] = {&sd->s_head,   &sd->s_neck,  &sd->s_body,  &sd->s_back,  &sd->s_hand_r,
                             &sd->s_hand_l, &sd->s_hands, &sd->s_arm_r, &sd->s_arm_l, &sd->s_feet};
    if (br_u32(&r) != (uint32_t)(sizeof(slots) / sizeof(slots[0]))) {
        r.failed = true;
    }
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]) && !r.failed; i++) {
        get_item(&r, slots[i]);
    }
    get_items(&r, sd->s_rings, 10);
    get_i32s(&r, sd->spell_slots, MAX_SPELL_LEVEL + 1);
    get_i32s(&r, sd->spell_slots_max, MAX_SPELL_LEVEL + 1);
    sd->alignment = br_i32(&r);
    sd->effect_count = (int)br_count(&r, MAX_EFFECTS_PER_ENTITY);
    for (int i = 0; i < sd->effect_count && !r.failed; i++) {
        SavedEffect *e = &sd->effects[i];
        e->cond = br_i32(&r);
        e->trigger = br_i32(&r);
        e->mod_type = br_i32(&r);
        e->value = br_i32(&r);
        e->duration_rounds = br_i32(&r);
        e->is_persistent = br_bool(&r) ? 1 : 0;
    }
    sd->light_turns_left = br_i32(&r);
    sd->hunger_level = br_i32(&r);
    sd->exhaustion_level = br_i32(&r);
    sd->total_kills = br_i32(&r);
    sd->total_steps = br_i32(&r);
    sd->max_floor_explored = br_i32(&r);
    if (br_u32(&r) != KNOWN_SPELLS_WORDS) {
        r.failed = true;
    }
    for (int i = 0; i < KNOWN_SPELLS_WORDS && !r.failed; i++) {
        sd->known_spells[i] = br_u64(&r);
    }
    sd->bosses_defeated = br_u32(&r);
    sd->unspent_stat_points = br_i32(&r);
    sd->blocked_count = (int)br_count(&r, MAX_BLOCKED_PLAYERS);
    for (int i = 0; i < sd->blocked_count && !r.failed; i++) {
        br_str(&r, sd->blocked_players[i], sizeof(sd->blocked_players[i]));
    }
    return finish_read(&r, buf, why, why_len);
}

/* --- World ---------------------------------------------------------------------- */

bool persist_write_world(const char *path, const World *wd) {
    BinWriter w;
    bw_init(&w);
    bw_u32(&w, MAX_FLOORS);
    bw_u32(&w, MAP_WIDTH);
    bw_u32(&w, MAP_HEIGHT);
    bw_u32(&w, MAP_DEPTH);
    for (int f = 0; f < MAX_FLOORS; f++) {
        const Floor *fl = &wd->floors[f];
        bw_i32(&w, fl->id);
        /* One byte per tile: VoxelType is uint8_t, no byte order involved. */
        bw_bytes(&w, fl->map.data, sizeof(fl->map.data));
        int traps = fl->trap_count < 0 ? 0 : fl->trap_count;
        if (traps > MAX_TRAPS_PER_FLOOR) {
            traps = MAX_TRAPS_PER_FLOOR;
        }
        bw_u32(&w, (uint32_t)traps);
        for (int t = 0; t < traps; t++) {
            const Trap *tr = &fl->traps[t];
            bw_i32(&w, (int32_t)tr->type);
            bw_i32(&w, tr->x);
            bw_i32(&w, tr->y);
            bw_i32(&w, tr->floor_id);
            bw_i32(&w, tr->detection_dc);
            bw_i32(&w, tr->save_dc);
            bw_i32(&w, tr->damage_dice);
            bw_i32(&w, tr->damage_sides);
            bw_i32(&w, (int32_t)tr->damage_type);
            bw_i32(&w, tr->respawn_timer);
            bw_bool(&w, tr->active);
            bw_bool(&w, tr->detected);
            bw_i32(&w, tr->wall_dir);
        }
        int crystals = fl->crystal_respawn_count < 0 ? 0 : fl->crystal_respawn_count;
        if (crystals > CRYSTAL_RESPAWN_MAX) {
            crystals = CRYSTAL_RESPAWN_MAX;
        }
        bw_u32(&w, (uint32_t)crystals);
        for (int c = 0; c < crystals; c++) {
            const CrystalRespawn *cr = &fl->crystal_respawns[c];
            bw_i32(&w, cr->x);
            bw_i32(&w, cr->y);
            bw_u8(&w, cr->type);
            bw_i32(&w, cr->respawn_timer);
        }
    }
    return write_and_free(path, PERSIST_WORLD_MAGIC, PERSIST_WORLD_VERSION, &w);
}

BinFileStatus persist_read_world(const char *path, World *wd, char *why, size_t why_len) {
    BinReader r;
    uint8_t *buf = NULL;
    BinFileStatus st =
        read_payload(path, PERSIST_WORLD_MAGIC, PERSIST_WORLD_VERSION, &r, &buf, why, why_len);
    if (st != BINFILE_OK) {
        return st;
    }
    if (br_u32(&r) != MAX_FLOORS || br_u32(&r) != MAP_WIDTH || br_u32(&r) != MAP_HEIGHT ||
        br_u32(&r) != MAP_DEPTH) {
        free(buf);
        if (why && why_len) {
            snprintf(why, why_len, "world dimensions differ from this build");
        }
        return BINFILE_INVALID;
    }
    for (int f = 0; f < MAX_FLOORS && !r.failed; f++) {
        Floor *fl = &wd->floors[f];
        fl->id = br_i32(&r);
        br_bytes(&r, fl->map.data, sizeof(fl->map.data));
        fl->trap_count = (int)br_count(&r, MAX_TRAPS_PER_FLOOR);
        for (int t = 0; t < fl->trap_count && !r.failed; t++) {
            Trap *tr = &fl->traps[t];
            memset(tr, 0, sizeof(*tr));
            tr->type = (TrapType)get_enum(&r, 0, TRAP_COUNT - 1);
            tr->x = br_i32(&r);
            tr->y = br_i32(&r);
            tr->floor_id = br_i32(&r);
            tr->detection_dc = br_i32(&r);
            tr->save_dc = br_i32(&r);
            tr->damage_dice = br_i32(&r);
            tr->damage_sides = br_i32(&r);
            tr->damage_type = (DamageType)get_enum(&r, 0, MAX_DAMAGE_TYPES - 1);
            tr->respawn_timer = br_i32(&r);
            tr->active = br_bool(&r);
            tr->detected = br_bool(&r);
            tr->wall_dir = br_i32(&r);
            if (tr->x < 0 || tr->x >= MAP_WIDTH || tr->y < 0 || tr->y >= MAP_HEIGHT) {
                r.failed = true;
            }
        }
        fl->crystal_respawn_count = (int)br_count(&r, CRYSTAL_RESPAWN_MAX);
        for (int c = 0; c < fl->crystal_respawn_count && !r.failed; c++) {
            CrystalRespawn *cr = &fl->crystal_respawns[c];
            cr->x = br_i32(&r);
            cr->y = br_i32(&r);
            cr->type = br_u8(&r);
            cr->respawn_timer = br_i32(&r);
            if (cr->x < 0 || cr->x >= MAP_WIDTH || cr->y < 0 || cr->y >= MAP_HEIGHT) {
                r.failed = true;
            }
        }
        /* The entity grid is runtime state, rebuilt by sync_entity_grid(). */
        memset(fl->entity_grid, 0, sizeof(fl->entity_grid));
    }
    return finish_read(&r, buf, why, why_len);
}

/* --- NPCs ------------------------------------------------------------------------ */

static bool npc_is_persistent(const NPC *n) {
    return n->active || n->template != NULL;
}

static void put_npc(BinWriter *w, uint32_t slot, const NPC *n) {
    bw_u32(w, slot);
    bw_i32(w, n->entity_id);
    bw_i32(w, n->x);
    bw_i32(w, n->y);
    bw_i32(w, n->floor_id);
    bw_i32(w, n->hp);
    bw_i32(w, n->max_hp);
    bw_bool(w, n->active);
    bw_i32(w, (int32_t)n->archetype);
    bw_i32(w, n->template_idx);
    bw_i32(w, n->ac);
    bw_i32(w, n->attack_bonus);
    bw_i32(w, n->damage_dice);
    bw_i32(w, n->damage_sides);
    bw_i32(w, n->xp_reward);
    bw_i32(w, n->gold_drop);
    bw_i32(w, n->morale);
    put_i32s(w, n->spell_slots, MAX_SPELL_LEVEL + 1);
    put_i32s(w, n->spell_slots_max, MAX_SPELL_LEVEL + 1);
    bw_i32(w, n->ai_ctx.current_target_id);
    bw_i32(w, n->ai_ctx.state_timer);
    bw_i32(w, n->ai_ctx.legendary_actions_spent);
    bw_i32(w, (int32_t)n->ai_ctx.behavior);
    const MerchantData *m = &n->merchant;
    int items = m->item_count;
    if (items < 0) {
        items = 0;
    }
    if (items > MAX_SHOP_ITEMS) {
        items = MAX_SHOP_ITEMS;
    }
    bw_u32(w, (uint32_t)items);
    for (int i = 0; i < items; i++) {
        bw_i32(w, m->item_templates[i]);
        bw_i32(w, m->item_stock[i]);
        bw_i32(w, m->item_stock_max[i]);
    }
    bw_i32(w, m->restock_timer);
    bw_str(w, m->shop_name, sizeof(m->shop_name));
    bw_i32(w, (int32_t)m->spec);
    bw_bool(w, n->is_ghost);
    bw_str(w, n->custom_name, sizeof(n->custom_name));
    put_sparse_items(w, n->ghost_loot, GHOST_LOOT_MAX);
    bw_i32(w, n->spawn_x);
    bw_i32(w, n->spawn_y);
    bw_i32(w, n->respawn_timer);
    bw_i32(w, n->energy);
}

static void get_npc(BinReader *r, NPC *n) {
    memset(n, 0, sizeof(*n));
    n->entity_id = br_i32(r);
    n->x = br_i32(r);
    n->y = br_i32(r);
    n->floor_id = br_i32(r);
    n->hp = br_i32(r);
    n->max_hp = br_i32(r);
    n->active = br_bool(r);
    n->archetype = (EntityArchetype)get_enum(r, 0, ARCH_COUNT - 1);
    n->template_idx = br_i32(r);
    n->ac = br_i32(r);
    n->attack_bonus = br_i32(r);
    n->damage_dice = br_i32(r);
    n->damage_sides = br_i32(r);
    n->xp_reward = br_i32(r);
    n->gold_drop = br_i32(r);
    n->morale = br_i32(r);
    get_i32s(r, n->spell_slots, MAX_SPELL_LEVEL + 1);
    get_i32s(r, n->spell_slots_max, MAX_SPELL_LEVEL + 1);
    n->ai_ctx.current_target_id = br_i32(r);
    n->ai_ctx.state_timer = br_i32(r);
    n->ai_ctx.legendary_actions_spent = br_i32(r);
    n->ai_ctx.behavior = (AiBehavior)get_enum(r, AI_BEHAVIOR_DEFAULT, AI_BEHAVIOR_SWARM);
    MerchantData *m = &n->merchant;
    m->item_count = (int)br_count(r, MAX_SHOP_ITEMS);
    for (int i = 0; i < m->item_count && !r->failed; i++) {
        m->item_templates[i] = br_i32(r);
        m->item_stock[i] = br_i32(r);
        m->item_stock_max[i] = br_i32(r);
    }
    m->restock_timer = br_i32(r);
    br_str(r, m->shop_name, sizeof(m->shop_name));
    m->spec = (MerchantSpecialization)get_enum(r, 0, SPEC_COUNT - 1);
    n->is_ghost = br_bool(r);
    br_str(r, n->custom_name, sizeof(n->custom_name));
    get_sparse_items(r, n->ghost_loot, GHOST_LOOT_MAX);
    n->spawn_x = br_i32(r);
    n->spawn_y = br_i32(r);
    n->respawn_timer = br_i32(r);
    n->energy = br_i32(r);
    if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS || n->x < 0 || n->x >= MAP_WIDTH || n->y < 0 ||
        n->y >= MAP_HEIGHT) {
        r->failed = true;
    }
}

bool persist_write_npcs(const char *path, const NPC *npcs, int max_npcs, int next_id,
                        int total_turns) {
    uint32_t count = 0;
    for (int i = 0; i < max_npcs; i++) {
        if (npc_is_persistent(&npcs[i])) {
            count++;
        }
    }
    BinWriter w;
    bw_init(&w);
    bw_u32(&w, (uint32_t)max_npcs);
    bw_u32(&w, bestiary_fingerprint());
    bw_i32(&w, next_id);
    bw_i32(&w, total_turns);
    bw_u32(&w, count);
    for (int i = 0; i < max_npcs; i++) {
        if (npc_is_persistent(&npcs[i])) {
            put_npc(&w, (uint32_t)i, &npcs[i]);
        }
    }
    return write_and_free(path, PERSIST_NPCS_MAGIC, PERSIST_NPCS_VERSION, &w);
}

BinFileStatus persist_read_npcs(const char *path, NPC *npcs, int max_npcs, int *next_id,
                                int *total_turns, int *loaded, char *why, size_t why_len) {
    BinReader r;
    uint8_t *buf = NULL;
    *loaded = 0;
    BinFileStatus st =
        read_payload(path, PERSIST_NPCS_MAGIC, PERSIST_NPCS_VERSION, &r, &buf, why, why_len);
    if (st != BINFILE_OK) {
        return st;
    }
    if (br_u32(&r) != (uint32_t)max_npcs) {
        free(buf);
        if (why && why_len) {
            snprintf(why, why_len, "NPC capacity differs from this build");
        }
        return BINFILE_INVALID;
    }
    if (br_u32(&r) != bestiary_fingerprint()) {
        free(buf);
        if (why && why_len) {
            snprintf(why, why_len, "written for another bestiary (data/bestiary.json changed)");
        }
        return BINFILE_INVALID;
    }
    int nid = br_i32(&r);
    int turns = br_i32(&r);
    uint32_t count = br_count(&r, (uint32_t)max_npcs);
    for (uint32_t k = 0; k < count && !r.failed; k++) {
        uint32_t slot = br_u32(&r);
        if (slot >= (uint32_t)max_npcs || npc_is_persistent(&npcs[slot]) ||
            npcs[slot].entity_id != 0) {
            r.failed = true; /* out of range or duplicated slot */
            break;
        }
        get_npc(&r, &npcs[slot]);
        (*loaded)++;
    }
    if (nid < 1) {
        r.failed = true;
    }
    st = finish_read(&r, buf, why, why_len);
    if (st == BINFILE_OK) {
        *next_id = nid;
        *total_turns = turns;
    }
    return st;
}

/* --- Tombstones ----------------------------------------------------------------- */

bool persist_write_tombstone(const char *path, const Tombstone *t) {
    BinWriter w;
    bw_init(&w);
    bw_bool(&w, t->active);
    bw_i32(&w, t->x);
    bw_i32(&w, t->y);
    bw_i32(&w, t->floor_id);
    bw_str(&w, t->owner, sizeof(t->owner));
    bw_u64(&w, t->gold);
    bw_i64(&w, (int64_t)t->death_time);
    bw_i32(&w, t->backpack_count);
    put_items(&w, t->backpack, TOMBSTONE_BACKPACK_SIZE);
    put_items(&w, t->belt, 4);
    const ItemInstance *slots[] = {&t->slot_head,   &t->slot_neck,   &t->slot_body,  &t->slot_back,
                                   &t->slot_hand_r, &t->slot_hand_l, &t->slot_hands, &t->slot_arm_r,
                                   &t->slot_arm_l,  &t->slot_feet};
    bw_u32(&w, (uint32_t)(sizeof(slots) / sizeof(slots[0])));
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        put_item(&w, slots[i]);
    }
    put_items(&w, t->slot_rings, TOMBSTONE_RINGS);
    return write_and_free(path, PERSIST_TOMB_MAGIC, PERSIST_TOMB_VERSION, &w);
}

BinFileStatus persist_read_tombstone(const char *path, Tombstone *t, char *why, size_t why_len) {
    BinReader r;
    uint8_t *buf = NULL;
    BinFileStatus st =
        read_payload(path, PERSIST_TOMB_MAGIC, PERSIST_TOMB_VERSION, &r, &buf, why, why_len);
    if (st != BINFILE_OK) {
        return st;
    }
    memset(t, 0, sizeof(*t));
    t->active = br_bool(&r);
    t->x = br_i32(&r);
    t->y = br_i32(&r);
    t->floor_id = br_i32(&r);
    br_str(&r, t->owner, sizeof(t->owner));
    t->gold = br_u64(&r);
    t->death_time = (time_t)br_i64(&r);
    t->backpack_count = br_i32(&r);
    get_items(&r, t->backpack, TOMBSTONE_BACKPACK_SIZE);
    get_items(&r, t->belt, 4);
    ItemInstance *slots[] = {&t->slot_head,   &t->slot_neck,   &t->slot_body,  &t->slot_back,
                             &t->slot_hand_r, &t->slot_hand_l, &t->slot_hands, &t->slot_arm_r,
                             &t->slot_arm_l,  &t->slot_feet};
    if (br_u32(&r) != (uint32_t)(sizeof(slots) / sizeof(slots[0]))) {
        r.failed = true;
    }
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]) && !r.failed; i++) {
        get_item(&r, slots[i]);
    }
    get_items(&r, t->slot_rings, TOMBSTONE_RINGS);
    if (t->backpack_count < 0 || t->backpack_count > TOMBSTONE_BACKPACK_SIZE) {
        r.failed = true;
    }
    return finish_read(&r, buf, why, why_len);
}

/* --- Flags (unique artifacts) ------------------------------------------------- */

bool persist_write_flags(const char *path, const bool *flags, int count) {
    BinWriter w;
    bw_init(&w);
    bw_u32(&w, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        bw_bool(&w, flags[i]);
    }
    return write_and_free(path, PERSIST_ART_MAGIC, PERSIST_ART_VERSION, &w);
}

BinFileStatus persist_read_flags(const char *path, bool *flags, int count, char *why,
                                 size_t why_len) {
    BinReader r;
    uint8_t *buf = NULL;
    BinFileStatus st =
        read_payload(path, PERSIST_ART_MAGIC, PERSIST_ART_VERSION, &r, &buf, why, why_len);
    if (st != BINFILE_OK) {
        return st;
    }
    if (br_u32(&r) != (uint32_t)count) {
        free(buf);
        if (why && why_len) {
            snprintf(why, why_len, "artifact count differs from this build");
        }
        return BINFILE_INVALID;
    }
    bool tmp[256];
    if (count > (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        free(buf);
        return BINFILE_INVALID;
    }
    for (int i = 0; i < count; i++) {
        tmp[i] = br_bool(&r);
    }
    st = finish_read(&r, buf, why, why_len);
    if (st == BINFILE_OK) {
        memcpy(flags, tmp, sizeof(bool) * (size_t)count);
    }
    return st;
}
