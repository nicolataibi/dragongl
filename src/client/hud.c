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
 * hud.c - The panels of the HUD (hud.h). Layout in virtual pixels, in
 * units of the character cell and of the text line (hud_draw.h).
 */

#include "hud.h"

#include "classes.h"
#include "client_log.h"
#include "client_minimap.h"
#include "species.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MARGIN       10.0f /*from the edge of the window, and between panels*/
#define STATUS_COLS  34
#define EQUIP_COLS   29
#define LOG_COLS_MAX 78
#define LOG_ROWS     8
#define LOG_ROWS_BIG 12 /*while typing or reading back*/

static int g_log_scroll_limit;

int hud_log_scroll_limit(void) {
    return g_log_scroll_limit;
}

/*Inner padding of a panel.*/
static float pad(const Hud *h) {
    return h->dot * 3.0f;
}

/*Row r of a panel whose inner corner is (x, y).*/
static void row_text(Hud *h, float x, float y, int r, const char *s, HudColor c) {
    hud_text(h, x, y + (float)r * h->line_h, s, hud_color(c), 1);
}

/* --- character --------------------------------------------------------------- */

static const char *hunger_word(HudColor *c) {
    if (g_hunger_level >= 1850) {
        *c = HUD_DANGER;
        return "FAINTING";
    }
    if (g_hunger_level >= 1600) {
        *c = HUD_DANGER;
        return "WEAK";
    }
    if (g_hunger_level >= 1200) {
        *c = HUD_WARN;
        return "HUNGRY";
    }
    *c = HUD_GOOD;
    return "SATIATED";
}

/*"HP" with a bar and the numbers: the state is never colour alone.*/
static void hp_row(Hud *h, float x, float y) {
    char buf[48];
    float ratio = g_my_max_hp > 0 ? (float)g_my_hp / (float)g_my_max_hp : 0.0f;
    ratio = fminf(fmaxf(ratio, 0.0f), 1.0f);
    HudColor c = HUD_DANGER;
    if (ratio > 0.5f) {
        c = HUD_GOOD;
    } else if (ratio > 0.25f) {
        c = HUD_WARN;
    }
    const Rgb empty = {0.20f, 0.22f, 0.26f};
    float bar_x = x + 3.0f * h->cell_w, bar_w = 12.0f * h->cell_w, bar_h = 7.0f * h->dot;
    hud_text(h, x, y, "HP", hud_color(HUD_DIM), 1);
    hud_quad(h, bar_x, y, bar_w, bar_h, empty, 1.0f);
    hud_quad(h, bar_x, y, bar_w * ratio, bar_h, hud_color(c), 1.0f);
    snprintf(buf, sizeof(buf), "%d/%d", g_my_hp, g_my_max_hp);
    hud_text(h, bar_x + bar_w + h->cell_w, y, buf, hud_color(c), 1);
}

/*Spell slots, three levels per row: "1:2/2 2:1/1". Returns the rows used.*/
static int slot_rows(Hud *h, float x, float y, int first_row) {
    char buf[96] = "";
    int rows = 0, in_row = 0;
    for (int lvl = 1; lvl <= 9; lvl++) {
        if (g_my_spell_slots_max[lvl] <= 0) {
            continue;
        }
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "%d:%d/%d ", lvl, g_my_spell_slots[lvl],
                 g_my_spell_slots_max[lvl]);
        if (++in_row == 3) {
            hud_text(h, x, y + (float)(first_row + rows) * h->line_h, rows ? "     " : "SLOTS",
                     hud_color(HUD_DIM), 1);
            hud_text(h, x + 6.0f * h->cell_w, y + (float)(first_row + rows) * h->line_h, buf,
                     hud_color(HUD_MAGIC), 1);
            rows++;
            in_row = 0;
            buf[0] = '\0';
        }
    }
    if (in_row > 0) {
        hud_text(h, x, y + (float)(first_row + rows) * h->line_h, rows ? "     " : "SLOTS",
                 hud_color(HUD_DIM), 1);
        hud_text(h, x + 6.0f * h->cell_w, y + (float)(first_row + rows) * h->line_h, buf,
                 hud_color(HUD_MAGIC), 1);
        rows++;
    }
    return rows;
}

/*Active conditions, as words. Returns the rows used.*/
static int condition_rows(Hud *h, float x, float y, int first_row) {
    static const char *const words[12] = {"POISON",      "BLIND",   "PARALYZED", "STUNNED",
                                          "UNCONSCIOUS", "BURNING", "BLEEDING",  "PETRIFIED",
                                          "CURSED",      "FROZEN",  "EXHAUSTED", "STUDYING"};
    char buf[STATUS_COLS + 1] = "";
    int rows = 0;
    for (int k = 0; k < 12; k++) {
        if (!(g_status_icons & (1u << k))) {
            continue;
        }
        if (strlen(buf) + strlen(words[k]) + 1 > STATUS_COLS) {
            row_text(h, x, y, first_row + rows++, buf, HUD_WARN);
            buf[0] = '\0';
        }
        size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, "%s ", words[k]);
    }
    if (buf[0]) {
        row_text(h, x, y, first_row + rows++, buf, HUD_WARN);
    }
    return rows;
}

/*Top left: who the character is and how it is doing. Returns the bottom
 * of the panel.*/
static float status_panel(Hud *h) {
    char buf[96];
    float x = MARGIN + pad(h), y = MARGIN + pad(h);
    int slots = 0, conds = 0;
    for (int lvl = 1; lvl <= 9; lvl++) {
        slots += g_my_spell_slots_max[lvl] > 0;
    }
    for (int k = 0; k < 12; k++) {
        conds += (g_status_icons & (1u << k)) != 0;
    }
    /*Rows: 9 fixed, the slots (3 levels a row), the conditions (at least
     * 2 words a row).*/
    int rows = 9 + (slots + 2) / 3 + (conds + 1) / 2;
    hud_panel(h, MARGIN, MARGIN, STATUS_COLS * h->cell_w + 2.0f * pad(h),
              (float)rows * h->line_h + 2.0f * pad(h) - 3.0f * h->dot);

    int r = 0;
    bool day = g_game_h >= 6 && g_game_h < 20;
    snprintf(buf, sizeof(buf), "%02d:%02d %s   FLOOR %d", g_game_h, g_game_m, day ? "DAY" : "NIGHT",
             g_my_floor);
    row_text(h, x, y, r++, buf, day ? HUD_WARN : HUD_INFO);
    const char *race = (g_race_id >= 0 && g_race_id < RACE_COUNT) ? RACES[g_race_id].name : "";
    const char *cls = (g_class_id >= 0 && g_class_id < CLASS_COUNT) ? CLASSES[g_class_id].name : "";
    snprintf(buf, sizeof(buf), "%.14s %.12s L%d", race, cls, g_my_level);
    row_text(h, x, y, r++, buf, HUD_TITLE);
    hp_row(h, x, y + (float)r++ * h->line_h);
    HudColor hc = HUD_GOOD;
    const char *hunger = hunger_word(&hc);
    row_text(h, x, y, r, "VITALITY", HUD_DIM);
    hud_text(h, x + 9.0f * h->cell_w, y + (float)r++ * h->line_h, hunger, hud_color(hc), 1);
    snprintf(buf, sizeof(buf), "XP %d   GOLD %lu", g_my_xp, (unsigned long)g_my_gold);
    row_text(h, x, y, r++, buf, HUD_GOLD);
    snprintf(buf, sizeof(buf), "AC %d   HIT %+d   DMG %+d", g_my_ac, g_to_hit, g_to_dmg);
    row_text(h, x, y, r++, buf, HUD_INFO);
    snprintf(buf, sizeof(buf), "STR %-3d DEX %-3d CON %-3d", g_str, g_dex, g_con);
    row_text(h, x, y, r++, buf, HUD_TEXT);
    snprintf(buf, sizeof(buf), "INT %-3d WIS %-3d CHA %-3d", g_intel, g_wis, g_cha);
    row_text(h, x, y, r++, buf, HUD_TEXT);
    snprintf(buf, sizeof(buf), "POS %d,%d", g_my_x, g_my_y);
    row_text(h, x, y, r++, buf, HUD_DIM);
    r += slot_rows(h, x, y, r);
    condition_rows(h, x, y, r);
    return MARGIN + (float)rows * h->line_h + 2.0f * pad(h);
}

/* --- minimap, equipment, trophies ------------------------------------------------ */

/*Top right. Rows of equal cells become one rectangle. Returns the bottom
 * of the panel.*/
static float minimap_panel(Hud *h) {
    float size = (float)MINIMAP_DISPLAY_SIZE;
    float ox = h->w - MARGIN - size - pad(h), oy = MARGIN + pad(h);
    float cell = size / (float)MINIMAP_BUF_SIZE;
    hud_panel(h, ox - pad(h), MARGIN, size + 2.0f * pad(h), size + 2.0f * pad(h));
    if (g_my_x < 0) {
        return MARGIN + size + 2.0f * pad(h);
    }
    /*The live map: the caller holds g_state_mutex, as the net thread does
     * when it writes it.*/
    minimap_update(g_my_x, g_my_y, g_local_map, g_vision_radius);
    for (int by = 0; by < MINIMAP_BUF_SIZE; by++) {
        int run = 0;
        for (int bx = 1; bx <= MINIMAP_BUF_SIZE; bx++) {
            const MiniPixel *a = &g_minimap_buf[by][run];
            if (bx < MINIMAP_BUF_SIZE &&
                memcmp(a, &g_minimap_buf[by][bx], sizeof(MiniPixel)) == 0) {
                continue;
            }
            if (a->a != 0) {
                Rgb c = {(float)a->r / 255.0f, (float)a->g / 255.0f, (float)a->b / 255.0f};
                hud_quad(h, ox + (float)run * cell, oy + (float)by * cell, (float)(bx - run) * cell,
                         cell, c, (float)a->a / 255.0f);
            }
            run = bx;
        }
    }
    /*The player, in the centre.*/
    const Rgb white = {1.0f, 1.0f, 1.0f};
    float c = (float)MINIMAP_RADIUS * cell;
    hud_quad(h, ox + c - cell, oy + c - cell, cell * 3.0f, cell * 3.0f, white, 1.0f);
    return MARGIN + size + 2.0f * pad(h);
}

static void equipment_panel(Hud *h, float top) {
    static const char *const labels[10] = {"HEAD",  "NECK",  "BODY",   "BACK",   "R.ARM",
                                           "L.ARM", "HANDS", "R.HAND", "L.HAND", "FEET"};
    const char *const names[10] = {g_eq_head,  g_eq_neck,  g_eq_body,   g_eq_back,   g_eq_arm_r,
                                   g_eq_arm_l, g_eq_hands, g_eq_hand_r, g_eq_hand_l, g_eq_feet};
    char buf[64];
    int extra = 0;
    for (int i = 0; i < 10; i++) {
        extra += g_eq_ring[i][0] != '\0';
    }
    for (int i = 0; i < 4; i++) {
        extra += g_eq_belt[i][0] != '\0';
    }
    float w = EQUIP_COLS * h->cell_w + 2.0f * pad(h);
    float x = h->w - MARGIN - w + pad(h), y = top + MARGIN + pad(h);
    hud_panel(h, x - pad(h), top + MARGIN, w,
              (float)(11 + extra) * h->line_h + 2.0f * pad(h) - 3.0f * h->dot);
    int r = 0;
    row_text(h, x, y, r++, "EQUIPMENT", HUD_TITLE);
    for (int i = 0; i < 10; i++) {
        row_text(h, x, y, r, labels[i], HUD_DIM);
        bool worn = names[i][0] != '\0';
        snprintf(buf, sizeof(buf), "%.22s", worn ? names[i] : "-");
        hud_text(h, x + 7.0f * h->cell_w, y + (float)r++ * h->line_h, buf,
                 hud_color(worn ? HUD_TEXT : HUD_DIM), 1);
    }
    for (int i = 0; i < 14; i++) {
        const char *name = i < 10 ? g_eq_ring[i] : g_eq_belt[i - 10];
        if (name[0] == '\0') {
            continue;
        }
        row_text(h, x, y, r, i < 10 ? "RING" : "BELT", HUD_DIM);
        snprintf(buf, sizeof(buf), "%.22s", name);
        hud_text(h, x + 7.0f * h->cell_w, y + (float)r++ * h->line_h, buf,
                 hud_color(i < 10 ? HUD_MAGIC : HUD_GOOD), 1);
    }
}

/*Bottom right: the bosses of floors 10 to 100. A defeated one is marked
 * with a star as well as with the colour. Returns the left edge.*/
static float trophies_panel(Hud *h) {
    char buf[8];
    float w = 5.0f * 5.0f * h->cell_w + 2.0f * pad(h);
    float ht = 3.0f * h->line_h + 2.0f * pad(h) - 3.0f * h->dot;
    float x = h->w - MARGIN - w + pad(h), y = h->h - MARGIN - ht + pad(h);
    hud_panel(h, x - pad(h), y - pad(h), w, ht);
    row_text(h, x, y, 0, "BOSS TROPHIES", HUD_TITLE);
    for (int i = 0; i < 10; i++) {
        bool won = (g_bosses_defeated & (1u << i)) != 0;
        snprintf(buf, sizeof(buf), "%d%s", (i + 1) * 10, won ? "*" : "");
        hud_text(h, x + (float)(i % 5) * 5.0f * h->cell_w, y + (float)(1 + (i >= 5)) * h->line_h,
                 buf, hud_color(won ? HUD_GOLD : HUD_DIM), 1);
    }
    return x - pad(h);
}

/* --- messages and command line ------------------------------------------------------ */

/*One row (by index) of a wrapped message.*/
static void wrapped_row(const char *text, int cols, int row, char *out, size_t cap) {
    int next = 0, n = 0;
    for (int r = 0; r <= row; r++) {
        n = text_wrap_row(text, cols, &next);
        if (r < row) {
            text += next;
        }
    }
    if ((size_t)n >= cap) {
        n = (int)cap - 1;
    }
    memcpy(out, text, (size_t)n);
    out[n] = '\0';
}

/*The newest rows of the log, bottom up, skipping the first scroll rows.*/
static void log_rows(Hud *h, float x, float bottom, int cols, int visible, int scroll) {
    char line[CLIENT_LOG_TEXT], row[CLIENT_LOG_TEXT];
    int seen = 0; /*rows met so far, newest first*/
    for (int back = 0; client_log_get(back, line, sizeof(line)); back++) {
        int rows = text_wrap_count(line, cols);
        HudColor c = strncmp(line, "> ", 2) == 0 ? HUD_DIM : hud_message_color(line);
        for (int r = rows - 1; r >= 0; r--, seen++) {
            int slot = seen - scroll;
            if (slot < 0 || slot >= visible) {
                continue;
            }
            wrapped_row(line, cols, r, row, sizeof(row));
            hud_text(h, x, bottom - (float)(slot + 1) * h->line_h, row, hud_color(c), 1);
        }
        if (seen >= scroll + visible) {
            /*Enough to fill the panel: count the rest only to know where
             * scrolling must stop.*/
            for (back++; client_log_get(back, line, sizeof(line)); back++) {
                seen += text_wrap_count(line, cols);
            }
            break;
        }
    }
    g_log_scroll_limit = seen > visible ? seen - visible : 0;
}

/*The command line: "> text" with a block cursor; the text slides so that
 * the cursor stays in view.*/
static void command_row(Hud *h, float x, float y, int cols, const LineEdit *le) {
    int room = cols - 2;
    int first = le->cursor >= room ? le->cursor - room + 1 : 0;
    char shown[LINE_EDIT_MAX];
    snprintf(shown, sizeof(shown), "%.*s", room, le->text + first);
    hud_text(h, x, y, ">", hud_color(HUD_TITLE), 1);
    float tx = x + 2.0f * h->cell_w;
    float cx = tx + (float)(le->cursor - first) * h->cell_w;
    hud_quad(h, cx - h->dot * 0.5f, y - h->dot, h->cell_w, 9.0f * h->dot, hud_color(HUD_TITLE),
             1.0f);
    hud_text(h, tx, y, shown, hud_color(HUD_TEXT), 1);
    /*The character under the cursor, dark on the block.*/
    if (le->cursor < le->len) {
        char under[2] = {le->text[le->cursor], '\0'};
        hud_text(h, cx, y, under, HUD_PANEL, 1);
    }
}

/*"Enter: command   F1: keys", with the keys the player has bound.*/
static void hint_row(Hud *h, float x, float y, const ClientConfig *cfg, int scroll) {
    char cmd[40], help[40], buf[160];
    key_chord_format(cfg->keys[ACT_COMMAND][0], cmd, sizeof(cmd));
    key_chord_format(cfg->keys[ACT_HELP][0], help, sizeof(help));
    if (scroll > 0) {
        char down[40];
        key_chord_format(cfg->keys[ACT_LOG_DOWN][0], down, sizeof(down));
        snprintf(buf, sizeof(buf), "-- older messages: %s for the newest --", down);
    } else {
        snprintf(buf, sizeof(buf), "%s: command   %s: keys", cmd, help);
    }
    hud_text(h, x, y, buf, hud_color(HUD_DIM), 1);
}

static void messages_panel(Hud *h, const ClientUi *ui, const ClientConfig *cfg, float right) {
    float width = right - MARGIN - MARGIN;
    int cols = (int)((width - 2.0f * pad(h)) / h->cell_w);
    if (cols > LOG_COLS_MAX) {
        cols = LOG_COLS_MAX;
    }
    if (cols < 16) {
        return; /*no room: a window narrower than the trophies*/
    }
    int scroll = ui->log_scroll < g_log_scroll_limit ? ui->log_scroll : g_log_scroll_limit;
    /*A third of the window at most (half while typing): on a short window
     * fewer rows.*/
    bool big = ui->typing || scroll > 0;
    int visible = big ? LOG_ROWS_BIG : LOG_ROWS;
    int fit = (int)((h->h * (big ? 0.5f : 0.3f)) / h->line_h) - 1;
    if (visible > fit) {
        visible = fit > 2 ? fit : 2;
    }
    float w = (float)cols * h->cell_w + 2.0f * pad(h);
    float ht = (float)(visible + 1) * h->line_h + 2.0f * pad(h) - 3.0f * h->dot;
    float y = h->h - MARGIN - ht;
    hud_panel(h, MARGIN, y, w, ht);
    float x = MARGIN + pad(h);
    float last = y + pad(h) + (float)visible * h->line_h;
    log_rows(h, x, last, cols, visible, scroll);
    if (ui->typing) {
        command_row(h, x, last, cols, ui->line);
    } else {
        hint_row(h, x, last, cfg, scroll);
    }
}

/* --- list of keys ------------------------------------------------------------------- */

static void help_panel(Hud *h, const ClientConfig *cfg) {
    enum { COLS = 58, KEY_COLS = 30 };
    static const char *const typing[] = {
        "While typing a command:",
        "  Enter          send        Escape   cancel",
        "  Up / Down      earlier commands",
        "  Ctrl+Left/Right  by word   Home / End",
        "  Ctrl+U / Ctrl+K  delete to the start / end",
        "Mouse: drag to turn the camera, wheel to zoom.",
    };
    int rows = 1 + ACT_COUNT + cfg->macro_count + 1 + (int)(sizeof(typing) / sizeof(typing[0]));
    float w = COLS * h->cell_w + 2.0f * pad(h);
    float ht = (float)rows * h->line_h + 2.0f * pad(h);
    float x = (h->w - w) * 0.5f, y = (h->h - ht) * 0.5f;
    if (y < MARGIN) {
        y = MARGIN;
    }
    /*Over the other panels: opaque, so that nothing shows through.*/
    hud_quad(h, x, y, w, ht, HUD_PANEL, 1.0f);
    hud_panel(h, x, y, w, ht);
    x += pad(h);
    y += pad(h);
    int r = 0;
    char a[40], b[40], keys[96];
    row_text(h, x, y, r++, "KEYS", HUD_TITLE);
    for (int act = 0; act < ACT_COUNT; act++) {
        key_chord_format(cfg->keys[act][0], a, sizeof(a));
        key_chord_format(cfg->keys[act][1], b, sizeof(b));
        if (cfg->keys[act][0].key == 0) {
            snprintf(keys, sizeof(keys), "(no key)");
        } else if (cfg->keys[act][1].key != 0) {
            snprintf(keys, sizeof(keys), "%s, %s", a, b);
        } else {
            snprintf(keys, sizeof(keys), "%s", a);
        }
        keys[KEY_COLS - 1] = '\0';
        row_text(h, x, y, r, keys, HUD_WARN);
        hud_text(h, x + KEY_COLS * h->cell_w, y + (float)r++ * h->line_h,
                 client_action_label((ClientAction)act), hud_color(HUD_TEXT), 1);
    }
    for (int i = 0; i < cfg->macro_count; i++) {
        key_chord_format(cfg->macros[i].chord, a, sizeof(a));
        row_text(h, x, y, r, a, HUD_WARN);
        snprintf(keys, sizeof(keys), "> %.24s", cfg->macros[i].command);
        hud_text(h, x + KEY_COLS * h->cell_w, y + (float)r++ * h->line_h, keys,
                 hud_color(HUD_MAGIC), 1);
    }
    r++;
    for (size_t i = 0; i < sizeof(typing) / sizeof(typing[0]); i++) {
        row_text(h, x, y, r++, typing[i], HUD_DIM);
    }
}

void hud_panels(Hud *h, const ClientUi *ui, const ClientConfig *cfg, float frame_ms) {
    status_panel(h);
    float below_map = minimap_panel(h);
    float right = h->w;
    if (ui->show_side) {
        equipment_panel(h, below_map);
        right = trophies_panel(h);
    }
    messages_panel(h, ui, cfg, right);
    if (ui->show_fps && frame_ms > 0.0f) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%.1f ms  %.0f fps", (double)frame_ms,
                 1000.0 / (double)frame_ms);
        hud_label(h, h->w * 0.5f, MARGIN + pad(h), buf, hud_color(HUD_TEXT), 1);
    }
    if (ui->show_help) {
        help_panel(h, cfg);
    }
}
