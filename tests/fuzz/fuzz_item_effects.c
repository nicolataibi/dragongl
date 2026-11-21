/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * fuzz_item_effects - one item of data/items.json (JSON text) -> the parser
 * of its "worn" and "use" effects and of its spell fields
 * (data_item_effects.c), and the text as dice ("2d4+2"). Whatever the
 * JSON, the parser must stay in its fields: masks, dice and enums that the
 * server indexes with.
 *
 * DGL_FUZZ_SEEDS=<dir> writes as seeds every item of items.json that has
 * effects or a spell.
 */

#include "fuzz_common.h"

#include "cJSON.h"
#include "data_internal.h"
#include "fixture.h"
#include "items.h"
#include "rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_seeds(const char *dir) {
    FILE *f = fopen(DGL_FUZZ_DATA_DIR "/items.json", "rb");
    if (!f) {
        exit(1);
    }
    static char buf[4 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    const cJSON *it = NULL;
    int k = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "items")) {
        if (!cJSON_GetObjectItem(it, "worn") && !cJSON_GetObjectItem(it, "use") &&
            !cJSON_GetObjectItem(it, "cast_spell")) {
            continue;
        }
        char *text = cJSON_PrintUnformatted(it);
        char name[32];
        snprintf(name, sizeof(name), "item-%03d", k++);
        fuzz_write_seed(dir, name, -1, text, strlen(text));
        cJSON_free(text);
    }
    fuzz_write_seed(dir, "dice", -1, "2d4+2", 5);
    cJSON_Delete(root);
}

/* The signature is libFuzzer's: argc and argv may be changed by the target. */
// NOLINTNEXTLINE(readability-non-const-parameter)
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    char state[300];
    snprintf(state, sizeof(state), "%s/state", fuzz_tmp_dir());
    if (fx_init(state, DGL_FUZZ_DATA_DIR) != 0) { /*the spells, for "immune_spell"*/
        exit(1);
    }
    const char *dir = fuzz_seed_dir();
    if (dir) {
        write_seeds(dir);
        exit(0);
    }
    return 0;
}

static bool mask_ok(uint32_t mask, int count) {
    return (mask >> count) == 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';
    ItemDice d = {0, 0, 0};
    if (item_parse_dice(text, &d) && (d.count < 0 || d.sides < 0)) {
        abort();
    }
    cJSON *item = cJSON_Parse(text);
    free(text);
    if (!item) {
        return 0;
    }
    static ItemTemplate t;
    memset(&t, 0, sizeof(t));
    item_parse_effects(&t, item);
    cJSON_Delete(item);
    const ItemWorn *w = &t.worn;
    const ItemUse *u = &t.use;
    if (!mask_ok(w->resist | w->immune | w->resist_choices | u->resist, MAX_DAMAGE_TYPES) ||
        !mask_ok(w->immune_conditions | u->immune_conditions | u->cure, MAX_CONDITIONS) ||
        w->on_hit_type > MAX_DAMAGE_TYPES || w->on_crit_type > MAX_DAMAGE_TYPES ||
        u->harm_type > MAX_DAMAGE_TYPES || u->condition > MAX_CONDITIONS ||
        w->immune_spell >= spell_database_size || t.charge_cost < 1 ||
        t.damage_type > MAX_DAMAGE_TYPES) {
        abort();
    }
    return 0;
}
