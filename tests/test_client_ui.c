/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 *
 * The parts of the client that need no window: the command line, the
 * configuration and its keys, the HUD scale and the contrast of its
 * colours.
 */

#include "client_config.h"
#include "client_log.h"
#include "hud_theme.h"
#include "line_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

static void type(LineEdit *le, const char *s) {
    for (; *s; s++) {
        line_edit_insert(le, (unsigned char)*s);
    }
}

/* --- command line ------------------------------------------------------------ */

static void test_editing(void) {
    static LineEdit le;
    line_edit_init(&le);
    type(&le, "cast firebolt");
    CHECK(strcmp(le.text, "cast firebolt") == 0 && le.cursor == 13 && le.len == 13, "typing");

    line_edit_home(&le);
    type(&le, "x ");
    CHECK(strcmp(le.text, "x cast firebolt") == 0 && le.cursor == 2, "insert at the start");
    line_edit_backspace(&le);
    line_edit_backspace(&le);
    line_edit_backspace(&le); /* nothing left before the cursor */
    CHECK(strcmp(le.text, "cast firebolt") == 0 && le.cursor == 0, "backspace at the start");

    line_edit_word_right(&le);
    CHECK(le.cursor == 5, "word right stops at 'firebolt' (%d)", le.cursor);
    line_edit_delete(&le);
    CHECK(strcmp(le.text, "cast irebolt") == 0, "delete under the cursor: %s", le.text);
    line_edit_insert(&le, 'F');
    line_edit_end(&le);
    line_edit_delete(&le); /* nothing after the cursor */
    line_edit_right(&le);
    CHECK(strcmp(le.text, "cast Firebolt") == 0 && le.cursor == le.len, "end of the line");
    line_edit_word_left(&le);
    CHECK(le.cursor == 5, "word left (%d)", le.cursor);
    line_edit_left(&le);
    line_edit_kill_after(&le);
    CHECK(strcmp(le.text, "cast") == 0 && le.cursor == 4, "kill after: '%s'", le.text);
    type(&le, " magic missile");
    line_edit_word_left(&le);
    line_edit_kill_before(&le);
    CHECK(strcmp(le.text, "missile") == 0 && le.cursor == 0, "kill before: '%s'", le.text);

    /* Only printable ASCII; never more than the protocol carries. */
    CHECK(!line_edit_insert(&le, '\n') && !line_edit_insert(&le, 0x7f) &&
              !line_edit_insert(&le, 0xe8) && !line_edit_insert(&le, 0x1F600),
          "a control or non-ASCII character was accepted");
    line_edit_clear(&le);
    for (int i = 0; i < 2 * LINE_EDIT_MAX; i++) {
        line_edit_insert(&le, 'a');
    }
    CHECK(le.len == LINE_EDIT_MAX - 1 && le.text[le.len] == '\0', "full line: %d", le.len);
    CHECK(!line_edit_insert(&le, 'b'), "a full line accepted a character");
}

static void test_history(void) {
    static LineEdit le;
    char out[LINE_EDIT_MAX];
    line_edit_init(&le);
    line_edit_history_prev(&le); /* empty history: nothing happens */
    CHECK(le.len == 0, "history of an empty line edit");

    type(&le, "  look  ");
    CHECK(line_edit_submit(&le, out, sizeof(out)) && strcmp(out, "look") == 0, "trimmed: '%s'",
          out);
    CHECK(le.len == 0 && le.cursor == 0, "the line is empty after a submit");
    type(&le, "   ");
    CHECK(!line_edit_submit(&le, out, sizeof(out)) && out[0] == '\0', "blank line submitted");
    type(&le, "who");
    line_edit_submit(&le, out, sizeof(out));
    type(&le, "who");
    line_edit_submit(&le, out, sizeof(out));
    CHECK(le.history_count == 2, "a repeated line is kept once (%d)", le.history_count);

    type(&le, "inv");
    line_edit_history_prev(&le);
    CHECK(strcmp(le.text, "who") == 0 && le.cursor == 3, "newest entry: '%s'", le.text);
    line_edit_history_prev(&le);
    line_edit_history_prev(&le); /* already at the oldest */
    CHECK(strcmp(le.text, "look") == 0, "oldest entry: '%s'", le.text);
    line_edit_history_next(&le);
    line_edit_history_next(&le);
    CHECK(strcmp(le.text, "inv") == 0, "the line being typed comes back: '%s'", le.text);
    line_edit_history_next(&le);
    CHECK(strcmp(le.text, "inv") == 0, "past the newest");

    /* An entry edited and sent is a new entry; the old one stays. */
    line_edit_history_prev(&le);
    type(&le, " am i");
    line_edit_submit(&le, out, sizeof(out));
    CHECK(strcmp(out, "who am i") == 0 && le.history_count == 3 &&
              strcmp(le.history[1], "who") == 0,
          "edited entry");

    /* The history holds the last LINE_EDIT_HISTORY lines. */
    for (int i = 0; i < LINE_EDIT_HISTORY + 10; i++) {
        char line[32];
        snprintf(line, sizeof(line), "say %d", i);
        type(&le, line);
        line_edit_submit(&le, out, sizeof(out));
    }
    CHECK(le.history_count == LINE_EDIT_HISTORY, "history size %d", le.history_count);
    CHECK(strcmp(le.history[0], "say 10") == 0, "oldest kept: '%s'", le.history[0]);
    line_edit_history_prev(&le);
    CHECK(strcmp(le.text, "say 73") == 0, "newest: '%s'", le.text);

    /* A short output buffer truncates, with a terminator. */
    line_edit_clear(&le);
    type(&le, "abcdefgh");
    char small[5];
    CHECK(line_edit_submit(&le, small, sizeof(small)) && strcmp(small, "abcd") == 0, "'%s'", small);
}

/* --- keys and configuration ------------------------------------------------------ */

static int g_warnings;
static char g_last_warning[200];

static void on_warn(int line, const char *message, void *user) {
    (void)user;
    g_warnings++;
    snprintf(g_last_warning, sizeof(g_last_warning), "%d: %s", line, message);
}

static void test_key_names(void) {
    static const struct {
        const char *name;
        int key, mods;
    } cases[] = {
        {"W", 'W', 0},
        {"w", 'W', 0},
        {"7", '7', 0},
        {"F1", 290, 0},
        {"F12", 301, 0},
        {"Up", 265, 0},
        {"Enter", 257, 0},
        {"Escape", 256, 0},
        {"PageUp", 266, 0},
        {"Slash", 47, 0},
        {"KP8", 328, 0},
        {"KPAdd", 334, 0},
        {"Shift+PageUp", 266, KEYMOD_SHIFT},
        {"Ctrl+Equal", 61, KEYMOD_CTRL},
        {"ctrl+alt+shift+f5", 294, KEYMOD_CTRL | KEYMOD_ALT | KEYMOD_SHIFT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KeyChord c = {-1, -1};
        CHECK(key_chord_parse(cases[i].name, &c) && c.key == cases[i].key &&
                  c.mods == cases[i].mods,
              "'%s' -> key %d mods %d", cases[i].name, c.key, c.mods);
        /* What is printed parses back to the same chord. */
        char text[40];
        KeyChord back = {-1, -1};
        key_chord_format(c, text, sizeof(text));
        CHECK(key_chord_parse(text, &back) && back.key == c.key && back.mods == c.mods,
              "'%s' printed as '%s'", cases[i].name, text);
    }
    static const char *const bad[] = {"", "F0", "F26", "Ctrl+", "Hyper+A", "WW", "KP10", "Up+W"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        KeyChord c;
        CHECK(!key_chord_parse(bad[i], &c), "'%s' accepted as a key", bad[i]);
    }
    KeyChord none = {5, 5};
    CHECK(key_chord_parse("none", &none) && none.key == 0, "none");
}

static void test_defaults(void) {
    static ClientConfig cfg;
    client_config_defaults(&cfg);
    CHECK(cfg.ui_scale == 0.0f && !cfg.reduce_motion && !cfg.show_fps && cfg.macro_count == 0,
          "default settings");
    /* Everything the client does has a key: it can be played without a
     * mouse. */
    for (int a = 0; a < ACT_COUNT; a++) {
        CHECK(cfg.keys[a][0].key != 0, "action %s has no default key", client_action_name(a));
        CHECK(client_action_name(a)[0] && client_action_label(a)[0], "action %d has no name", a);
        for (int k = 0; k < CONFIG_KEYS_PER_ACTION; k++) {
            KeyChord c = cfg.keys[a][k];
            CHECK(c.key == 0 || client_config_action(&cfg, c.key, c.mods) == (ClientAction)a,
                  "a default key of %s is taken by another action", client_action_name(a));
        }
    }
    CHECK(client_config_action(&cfg, 'W', 0) == ACT_MOVE_NORTH, "W");
    CHECK(client_config_action(&cfg, 265, 0) == ACT_MOVE_NORTH, "Up");
    CHECK(client_config_action(&cfg, 266, 0) == ACT_ZOOM_IN, "PageUp");
    CHECK(client_config_action(&cfg, 266, KEYMOD_SHIFT) == ACT_LOG_UP, "Shift+PageUp");
    CHECK(client_config_action(&cfg, 'W', KEYMOD_CTRL) == ACT_NONE, "Ctrl+W is not W");
    /* Caps Lock and Num Lock (GLFW bits 0x10, 0x20) do not change a key. */
    CHECK(client_config_action(&cfg, 'W', 0x30) == ACT_MOVE_NORTH, "W with Caps Lock");
    CHECK(client_config_action(&cfg, 'Z', 0) == ACT_NONE, "Z is free");

    /* The printed defaults are a file that parses back to the defaults. */
    char *text = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&text, &size);
    client_config_print(&cfg, mem);
    fclose(mem);
    static ClientConfig back;
    client_config_defaults(&back);
    memset(back.keys, 0, sizeof(back.keys));
    g_warnings = 0;
    CHECK(client_config_parse(&back, text, on_warn, NULL) == 0, "printed defaults refused: %s",
          g_last_warning);
    CHECK(memcmp(back.keys, cfg.keys, sizeof(cfg.keys)) == 0, "printed keys differ");
    free(text);
}

static void test_config_file(void) {
    static ClientConfig cfg;
    client_config_defaults(&cfg);
    g_warnings = 0;
    int refused = client_config_parse(&cfg,
                                      "# my settings\n"
                                      "ui_scale = 1.6   # rounded to a quarter\n"
                                      "reduce_motion=yes\n"
                                      "  show_fps = on\r\n"
                                      "key.move_north = K, KP8\n"
                                      "key.quit = none\n"
                                      "bind.F5 = look\n"
                                      "bind.Ctrl+I = inventory\n"
                                      "\n",
                                      on_warn, NULL);
    CHECK(refused == 0 && g_warnings == 0, "a valid file was refused: %s", g_last_warning);
    CHECK(cfg.ui_scale == 1.5f, "ui_scale %.2f", (double)cfg.ui_scale);
    CHECK(cfg.reduce_motion && cfg.show_fps, "booleans");
    CHECK(client_config_action(&cfg, 'K', 0) == ACT_MOVE_NORTH &&
              client_config_action(&cfg, 328, 0) == ACT_MOVE_NORTH &&
              client_config_action(&cfg, 'W', 0) == ACT_NONE,
          "rebound action");
    CHECK(client_config_action(&cfg, 256, 0) == ACT_NONE, "unbound quit");
    CHECK(client_config_action(&cfg, 'S', 0) == ACT_MOVE_SOUTH, "an action not in the file");
    const char *m = client_config_macro(&cfg, 294, 0);
    CHECK(m && strcmp(m, "look") == 0, "macro F5");
    m = client_config_macro(&cfg, 'I', KEYMOD_CTRL);
    CHECK(m && strcmp(m, "inventory") == 0, "macro Ctrl+I");
    CHECK(client_config_macro(&cfg, 'I', 0) == NULL, "I without Ctrl");

    /* Every bad line is reported with its number and changes nothing. */
    static const char *const bad[] = {
        "ui_scale = huge",       "ui_scale = 9",     "ui_scale = 0.1",
        "reduce_motion = maybe", "key.fly = F",      "key.move_north = W, Up, K",
        "key.move_north = Nope", "bind.Nope = look", "bind.F6 =",
        "colour = red",          "just words",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        static ClientConfig c, before;
        client_config_defaults(&c);
        memcpy(&before, &c, sizeof(c)); /* with the padding: compared as bytes */
        char text[128];
        snprintf(text, sizeof(text), "\n\n%s\n", bad[i]);
        g_warnings = 0;
        CHECK(client_config_parse(&c, text, on_warn, NULL) == 1 && g_warnings == 1, "'%s' accepted",
              bad[i]);
        CHECK(strncmp(g_last_warning, "3:", 2) == 0, "'%s': warning '%s'", bad[i], g_last_warning);
        /* NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) */
        CHECK(memcmp(&c, &before, sizeof(c)) == 0, "'%s' changed the settings", bad[i]);
    }

    /* A key bound twice does the first thing; it is reported. */
    client_config_defaults(&cfg);
    g_warnings = 0;
    refused = client_config_parse(&cfg, "key.zoom_in = W\nbind.S = look\n", on_warn, NULL);
    CHECK(refused == 2 && g_warnings == 2, "conflicts reported: %d", refused);
    CHECK(client_config_action(&cfg, 'W', 0) == ACT_MOVE_NORTH, "the first binding wins");

    /* Too many macros. */
    client_config_defaults(&cfg);
    char many[4096] = "";
    for (int i = 0; i <= CONFIG_MAX_MACROS; i++) {
        char line[40];
        snprintf(line, sizeof(line), "bind.Alt+F%d = say %d\n", 1 + i % 25, i);
        if (i >= 25) {
            snprintf(line, sizeof(line), "bind.Ctrl+Alt+F%d = say %d\n", 1 + i % 25, i);
        }
        size_t used = strlen(many);
        snprintf(many + used, sizeof(many) - used, "%s", line);
    }
    CHECK(client_config_parse(&cfg, many, NULL, NULL) == 1 && cfg.macro_count == CONFIG_MAX_MACROS,
          "macro limit: %d", cfg.macro_count);
}

static void test_ui_scale(void) {
    CHECK(ui_scale_auto(1.0f, 800) == 1.0f, "800 px: %.2f", (double)ui_scale_auto(1.0f, 800));
    CHECK(ui_scale_auto(1.0f, 1080) == 1.25f, "1080 px");
    CHECK(ui_scale_auto(1.0f, 1728) == 2.25f, "1728 px");
    CHECK(ui_scale_auto(1.0f, 480) == 0.75f, "480 px: never under 0.75");
    CHECK(ui_scale_auto(2.0f, 1600) == 2.0f, "HiDPI, 800 logical pixels");
    CHECK(ui_scale_auto(2.0f, 960) == 1.5f, "HiDPI, a small window keeps readable text");
    CHECK(ui_scale_auto(1.0f, 100000) == UI_SCALE_MAX && ui_scale_auto(1.0f, 0) == 0.75f, "range");
    CHECK(ui_scale_auto(0.0f, 800) == 1.0f, "a content scale of 0 is 1");
    CHECK(ui_scale_clamp(1.13f) == 1.25f && ui_scale_clamp(1.12f) == 1.0f &&
              ui_scale_clamp(0.1f) == UI_SCALE_MIN && ui_scale_clamp(99.0f) == UI_SCALE_MAX,
          "steps of 0.25");
}

/* --- message log -------------------------------------------------------------------- */

static void test_log(void) {
    char line[CLIENT_LOG_TEXT];
    client_log_clear();
    CHECK(client_log_count() == 0 && !client_log_get(0, line, sizeof(line)), "empty log");
    client_log_add("first");
    client_log_add("second \t na\xc3\xafve");
    CHECK(client_log_count() == 2 && client_log_total() == 2, "two lines");
    CHECK(client_log_get(0, line, sizeof(line)) && strcmp(line, "second ? na??ve") == 0,
          "what the font cannot draw becomes '?': '%s'", line);
    CHECK(client_log_get(1, line, sizeof(line)) && strcmp(line, "first") == 0, "older line");
    CHECK(!client_log_get(2, line, sizeof(line)) && !client_log_get(-1, line, sizeof(line)),
          "outside the log");
    char small[4];
    CHECK(client_log_get(1, small, sizeof(small)) && strcmp(small, "fir") == 0, "short buffer");

    /* The oldest lines leave when the log is full. */
    for (int i = 0; i < CLIENT_LOG_LINES + 5; i++) {
        snprintf(line, sizeof(line), "line %d", i);
        client_log_add(line);
    }
    CHECK(client_log_count() == CLIENT_LOG_LINES, "full log: %d", client_log_count());
    CHECK(client_log_total() == (unsigned long)CLIENT_LOG_LINES + 7, "total");
    client_log_get(0, line, sizeof(line));
    CHECK(strcmp(line, "line 260") == 0, "newest: %s", line);
    client_log_get(CLIENT_LOG_LINES - 1, line, sizeof(line));
    CHECK(strcmp(line, "line 5") == 0, "oldest: %s", line);
    /* A line longer than the slot is cut, with its terminator. */
    char longer[2 * CLIENT_LOG_TEXT];
    memset(longer, 'x', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    client_log_add(longer);
    client_log_get(0, line, sizeof(line));
    CHECK(strlen(line) == CLIENT_LOG_TEXT - 1, "long line: %zu", strlen(line));
}

static void test_wrap(void) {
    int next = -1;
    CHECK(text_wrap_row("", 10, &next) == 0 && next == 0, "empty text");
    CHECK(text_wrap_row("short", 10, &next) == 5 && next == 5, "fits");
    CHECK(text_wrap_row("exactly ten", 11, &next) == 11 && next == 11, "fits exactly");
    /* Breaks at the last blank that fits; the blanks are not carried over. */
    CHECK(text_wrap_row("the quick brown fox", 10, &next) == 9 && next == 10,
          "break at a blank: %d", next);
    CHECK(text_wrap_row("the quick   brown", 10, &next) == 9 && next == 12, "several blanks: %d",
          next);
    /* A blank right after the last column. */
    CHECK(text_wrap_row("0123456789 abc", 10, &next) == 10 && next == 11, "blank at the edge");
    /* A word longer than the row is cut. */
    CHECK(text_wrap_row("abcdefghijklmnop", 10, &next) == 10 && next == 10, "long word");
    CHECK(text_wrap_count("", 10) == 1 && text_wrap_count("short", 10) == 1, "one row");
    CHECK(text_wrap_count("the quick brown fox jumps", 10) == 3, "three rows: %d",
          text_wrap_count("the quick brown fox jumps", 10));
    CHECK(text_wrap_count("abcdefghijklmnopqrstuvwxyz", 10) == 3, "long word rows");
    CHECK(text_wrap_count("abc", 0) == 3, "a column of no width is one character wide");
    /* Every character but the blanks at the breaks is in exactly one row. */
    const char *text = "[SYSTEM] You died! The Arcane has returned you to town without your "
                       "equipment!";
    char joined[200] = "";
    for (const char *p = text; *p;) {
        int n = text_wrap_row(p, 24, &next);
        CHECK(n <= 24 && n > 0, "row of %d characters", n);
        size_t used = strlen(joined);
        snprintf(joined + used, sizeof(joined) - used, "%.*s ", n, p);
        p += next;
    }
    joined[strlen(joined) - 1] = '\0';
    CHECK(strcmp(joined, text) == 0, "wrapped text reads '%s'", joined);
}

/* --- colours ------------------------------------------------------------------------ */

static void test_contrast(void) {
    const Rgb black = {0.0f, 0.0f, 0.0f}, white = {1.0f, 1.0f, 1.0f};
    CHECK(rgb_contrast(black, white) > 20.99 && rgb_contrast(black, white) < 21.01,
          "black on white is 21:1 (%.2f)", rgb_contrast(black, white));
    CHECK(rgb_contrast(white, white) == 1.0, "a colour on itself is 1:1");
    /* Each text colour on the panel, over the darkest and the brightest
     * thing the scene can put behind it. */
    const Rgb worst[] = {hud_panel_over(black), hud_panel_over(white)};
    for (int c = 0; c < HUD_COLOR_COUNT; c++) {
        for (int k = 0; k < 2; k++) {
            double ratio = rgb_contrast(hud_color((HudColor)c), worst[k]);
            CHECK(ratio >= HUD_MIN_CONTRAST, "%s on the panel over %s: %.2f:1, under %.1f:1",
                  hud_color_name((HudColor)c), k ? "white" : "black", ratio, HUD_MIN_CONTRAST);
        }
    }
    /* Tags of the server pick a colour; anything else is ordinary text. */
    CHECK(hud_message_color("[COMBAT] You hit the rat") == HUD_DAMAGE_OUT, "combat");
    CHECK(hud_message_color("[ERROR] No such item") == HUD_DANGER, "error");
    CHECK(hud_message_color("You see nothing") == HUD_TEXT, "plain");
    CHECK(hud_message_color("") == HUD_TEXT, "empty");
}

int main(void) {
    test_editing();
    test_history();
    test_key_names();
    test_defaults();
    test_config_file();
    test_ui_scale();
    test_log();
    test_wrap();
    test_contrast();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("client UI: ok\n");
    return 0;
}
