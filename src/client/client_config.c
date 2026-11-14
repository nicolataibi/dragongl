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
 * client_config.c - Settings and keys of the client (client_config.h).
 */

#include "client_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- key names ------------------------------------------------------------- */

typedef struct {
    const char *name;
    int key;
} KeyName;

/*The GLFW key codes (GLFW_KEY_*); letters, digits, F1..F25 and the keypad
 * digits are ranges and are handled apart.*/
static const KeyName k_key_names[] = {
    {"Space", 32},       {"Apostrophe", 39},  {"Comma", 44},        {"Minus", 45},
    {"Period", 46},      {"Slash", 47},       {"Semicolon", 59},    {"Equal", 61},
    {"LeftBracket", 91}, {"Backslash", 92},   {"RightBracket", 93}, {"Grave", 96},
    {"Escape", 256},     {"Enter", 257},      {"Tab", 258},         {"Backspace", 259},
    {"Insert", 260},     {"Delete", 261},     {"Right", 262},       {"Left", 263},
    {"Down", 264},       {"Up", 265},         {"PageUp", 266},      {"PageDown", 267},
    {"Home", 268},       {"End", 269},        {"KPDecimal", 330},   {"KPDivide", 331},
    {"KPMultiply", 332}, {"KPSubtract", 333}, {"KPAdd", 334},       {"KPEnter", 335},
};
#define KEY_NAME_COUNT (sizeof(k_key_names) / sizeof(k_key_names[0]))
enum { KEY_F1 = 290, KEY_F_LAST = 314, KEY_KP0 = 320 };

static int key_from_name(const char *s) {
    size_t n = strlen(s);
    if (n == 1 && isalpha((unsigned char)s[0])) {
        return toupper((unsigned char)s[0]);
    }
    if (n == 1 && isdigit((unsigned char)s[0])) {
        return s[0];
    }
    if ((s[0] == 'F' || s[0] == 'f') && n >= 2 && n <= 3 && isdigit((unsigned char)s[1]) &&
        (n == 2 || isdigit((unsigned char)s[2]))) {
        int f = (s[1] - '0') * (n == 3 ? 10 : 1) + (n == 3 ? s[2] - '0' : 0);
        return f >= 1 && KEY_F1 + f - 1 <= KEY_F_LAST ? KEY_F1 + f - 1 : 0;
    }
    if (n == 3 && strncasecmp(s, "KP", 2) == 0 && isdigit((unsigned char)s[2])) {
        return KEY_KP0 + (s[2] - '0');
    }
    for (size_t i = 0; i < KEY_NAME_COUNT; i++) {
        if (strcasecmp(s, k_key_names[i].name) == 0) {
            return k_key_names[i].key;
        }
    }
    return 0;
}

static void key_to_name(int key, char *out, size_t cap) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        snprintf(out, cap, "%c", key);
    } else if (key >= KEY_F1 && key <= KEY_F_LAST) {
        snprintf(out, cap, "F%d", key - KEY_F1 + 1);
    } else if (key >= KEY_KP0 && key <= KEY_KP0 + 9) {
        snprintf(out, cap, "KP%d", key - KEY_KP0);
    } else {
        for (size_t i = 0; i < KEY_NAME_COUNT; i++) {
            if (k_key_names[i].key == key) {
                snprintf(out, cap, "%s", k_key_names[i].name);
                return;
            }
        }
        snprintf(out, cap, "none");
    }
}

bool key_chord_parse(const char *s, KeyChord *out) {
    KeyChord c = {0, 0};
    if (strcasecmp(s, "none") == 0) {
        *out = c;
        return true;
    }
    for (;;) {
        if (strncasecmp(s, "Shift+", 6) == 0) {
            c.mods |= KEYMOD_SHIFT;
            s += 6;
        } else if (strncasecmp(s, "Ctrl+", 5) == 0) {
            c.mods |= KEYMOD_CTRL;
            s += 5;
        } else if (strncasecmp(s, "Alt+", 4) == 0) {
            c.mods |= KEYMOD_ALT;
            s += 4;
        } else {
            break;
        }
    }
    c.key = key_from_name(s);
    if (c.key == 0) {
        return false;
    }
    *out = c;
    return true;
}

void key_chord_format(KeyChord chord, char *out, size_t cap) {
    char name[24];
    key_to_name(chord.key, name, sizeof(name));
    if (chord.key == 0) {
        snprintf(out, cap, "none");
        return;
    }
    snprintf(out, cap, "%s%s%s%s", (chord.mods & KEYMOD_CTRL) ? "Ctrl+" : "",
             (chord.mods & KEYMOD_ALT) ? "Alt+" : "", (chord.mods & KEYMOD_SHIFT) ? "Shift+" : "",
             name);
}

/* --- actions --------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *label;
    const char *keys; /*defaults, as in the file*/
} ActionInfo;

static const ActionInfo k_actions[ACT_COUNT] = {
    [ACT_MOVE_NORTH] = {"move_north", "Step north", "W, Up"},
    [ACT_MOVE_SOUTH] = {"move_south", "Step south", "S, Down"},
    [ACT_MOVE_WEST] = {"move_west", "Step west", "A, Left"},
    [ACT_MOVE_EAST] = {"move_east", "Step east", "D, Right"},
    [ACT_ZOOM_IN] = {"zoom_in", "Camera closer", "PageUp, KPAdd"},
    [ACT_ZOOM_OUT] = {"zoom_out", "Camera farther", "PageDown, KPSubtract"},
    [ACT_CAMERA_LEFT] = {"camera_left", "Turn the camera left", "Q"},
    [ACT_CAMERA_RIGHT] = {"camera_right", "Turn the camera right", "E"},
    [ACT_CAMERA_UP] = {"camera_up", "Camera higher", "R"},
    [ACT_CAMERA_DOWN] = {"camera_down", "Camera lower", "F"},
    [ACT_COMMAND] = {"command", "Type a command", "Enter, Slash"},
    [ACT_LOG_UP] = {"log_up", "Older messages", "Shift+PageUp"},
    [ACT_LOG_DOWN] = {"log_down", "Newer messages", "Shift+PageDown"},
    [ACT_SCALE_UP] = {"scale_up", "Larger HUD", "Ctrl+Equal, Ctrl+KPAdd"},
    [ACT_SCALE_DOWN] = {"scale_down", "Smaller HUD", "Ctrl+Minus, Ctrl+KPSubtract"},
    [ACT_TOGGLE_FPS] = {"toggle_fps", "Frame time on/off", "F3"},
    [ACT_TOGGLE_PANELS] = {"toggle_panels", "Equipment and trophies on/off", "Tab"},
    [ACT_HELP] = {"help", "This list of keys", "F1"},
    [ACT_QUIT] = {"quit", "Leave the game", "Escape"},
};

const char *client_action_name(ClientAction a) {
    return a >= 0 && a < ACT_COUNT ? k_actions[a].name : "";
}

const char *client_action_label(ClientAction a) {
    return a >= 0 && a < ACT_COUNT ? k_actions[a].label : "";
}

/*"W, Up" into the slots of one action. False at the first name that is
 * not a key, or with more keys than slots.*/
static bool parse_key_list(const char *value, KeyChord out[CONFIG_KEYS_PER_ACTION]) {
    KeyChord keys[CONFIG_KEYS_PER_ACTION] = {{0, 0}};
    int n = 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", value);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
        KeyChord c;
        if (n >= CONFIG_KEYS_PER_ACTION || !key_chord_parse(tok, &c)) {
            return false;
        }
        if (c.key != 0) {
            keys[n++] = c;
        }
    }
    memcpy(out, keys, sizeof(keys));
    return true;
}

void client_config_defaults(ClientConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    for (int a = 0; a < ACT_COUNT; a++) {
        (void)parse_key_list(k_actions[a].keys, cfg->keys[a]);
    }
}

static bool same_chord(KeyChord c, int key, int mods) {
    return c.key != 0 && c.key == key && c.mods == (mods & KEYMOD_MASK);
}

ClientAction client_config_action(const ClientConfig *cfg, int key, int mods) {
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int k = 0; k < CONFIG_KEYS_PER_ACTION; k++) {
            if (same_chord(cfg->keys[a][k], key, mods)) {
                return (ClientAction)a;
            }
        }
    }
    return ACT_NONE;
}

const char *client_config_macro(const ClientConfig *cfg, int key, int mods) {
    for (int i = 0; i < cfg->macro_count; i++) {
        if (same_chord(cfg->macros[i].chord, key, mods)) {
            return cfg->macros[i].command;
        }
    }
    return NULL;
}

/* --- scale ----------------------------------------------------------------- */

float ui_scale_clamp(float scale) {
    float steps = (float)(int)(scale * 4.0f + 0.5f);
    float s = steps / 4.0f;
    if (s < UI_SCALE_MIN) {
        s = UI_SCALE_MIN;
    }
    if (s > UI_SCALE_MAX) {
        s = UI_SCALE_MAX;
    }
    return s;
}

float ui_scale_auto(float content_scale, int framebuffer_height) {
    if (content_scale < 1.0f) {
        content_scale = 1.0f;
    }
    /*The HUD keeps its share of the window: 1 at 800 pixels of height.
     * On a HiDPI monitor a small window must not shrink the text below
     * what is readable there.*/
    float s = (float)framebuffer_height / 800.0f;
    float floor_s = content_scale * 0.75f;
    return ui_scale_clamp(s > floor_s ? s : floor_s);
}

/* --- the file -------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

static bool parse_bool(const char *v, bool *out) {
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
        !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off") ||
        !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static const char *set_scale(ClientConfig *cfg, const char *value) {
    if (strcasecmp(value, "auto") == 0) {
        cfg->ui_scale = 0.0f;
        return NULL;
    }
    char *end = NULL;
    float s = strtof(value, &end);
    if (end == value || *end != '\0' || !(s >= UI_SCALE_MIN && s <= UI_SCALE_MAX)) {
        return "ui_scale is auto or a number from 0.5 to 4";
    }
    cfg->ui_scale = ui_scale_clamp(s);
    return NULL;
}

static const char *set_macro(ClientConfig *cfg, const char *key_name, const char *value) {
    KeyChord chord;
    if (!key_chord_parse(key_name, &chord) || chord.key == 0) {
        return "unknown key";
    }
    if (strlen(value) >= CONFIG_MACRO_LEN) {
        return "command too long";
    }
    for (int i = 0; i < cfg->macro_count; i++) {
        if (same_chord(cfg->macros[i].chord, chord.key, chord.mods)) {
            snprintf(cfg->macros[i].command, CONFIG_MACRO_LEN, "%s", value);
            return NULL;
        }
    }
    if (value[0] == '\0') {
        return "no command after the key";
    }
    if (cfg->macro_count >= CONFIG_MAX_MACROS) {
        return "too many bind lines";
    }
    cfg->macros[cfg->macro_count].chord = chord;
    snprintf(cfg->macros[cfg->macro_count].command, CONFIG_MACRO_LEN, "%s", value);
    cfg->macro_count++;
    return NULL;
}

/*One "name = value"; NULL, or why it is refused.*/
static const char *set_option(ClientConfig *cfg, const char *name, const char *value) {
    if (strcmp(name, "ui_scale") == 0) {
        return set_scale(cfg, value);
    }
    if (strcmp(name, "reduce_motion") == 0) {
        return parse_bool(value, &cfg->reduce_motion) ? NULL : "yes or no expected";
    }
    if (strcmp(name, "show_fps") == 0) {
        return parse_bool(value, &cfg->show_fps) ? NULL : "yes or no expected";
    }
    if (strncmp(name, "key.", 4) == 0) {
        for (int a = 0; a < ACT_COUNT; a++) {
            if (strcmp(name + 4, k_actions[a].name) == 0) {
                return parse_key_list(value, cfg->keys[a]) ? NULL
                                                           : "unknown key, or more than two keys";
            }
        }
        return "unknown action";
    }
    if (strncmp(name, "bind.", 5) == 0) {
        return set_macro(cfg, name + 5, value);
    }
    return "unknown setting";
}

/*A key bound twice does the first thing it is bound to: say so.*/
static int report_conflicts(const ClientConfig *cfg, ConfigWarn warn, void *user) {
    int found = 0;
    char msg[160], name[40];
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int k = 0; k < CONFIG_KEYS_PER_ACTION; k++) {
            KeyChord c = cfg->keys[a][k];
            if (c.key == 0) {
                continue;
            }
            key_chord_format(c, name, sizeof(name));
            ClientAction first = client_config_action(cfg, c.key, c.mods);
            if (first != (ClientAction)a) {
                snprintf(msg, sizeof(msg), "%s is bound to %s and to %s: it does %s", name,
                         k_actions[first].name, k_actions[a].name, k_actions[first].name);
                found++;
                if (warn) {
                    warn(0, msg, user);
                }
            }
        }
    }
    for (int i = 0; i < cfg->macro_count; i++) {
        KeyChord c = cfg->macros[i].chord;
        ClientAction act = client_config_action(cfg, c.key, c.mods);
        if (act != ACT_NONE) {
            key_chord_format(c, name, sizeof(name));
            snprintf(msg, sizeof(msg), "%s is bound to %s: its bind line is ignored", name,
                     k_actions[act].name);
            found++;
            if (warn) {
                warn(0, msg, user);
            }
        }
    }
    return found;
}

int client_config_parse(ClientConfig *cfg, const char *text, ConfigWarn warn, void *user) {
    int refused = 0, line_no = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        size_t take = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = '\0';
        p += eol ? n + 1 : n;
        line_no++;

        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *s = trim(line);
        if (*s == '\0') {
            continue;
        }
        const char *why = "no '=' in the line";
        char *eq = strchr(s, '=');
        if (n >= sizeof(line)) {
            why = "line too long";
        } else if (eq) {
            *eq = '\0';
            why = set_option(cfg, trim(s), trim(eq + 1));
        }
        if (why) {
            refused++;
            if (warn) {
                warn(line_no, why, user);
            }
        }
    }
    return refused + report_conflicts(cfg, warn, user);
}

bool client_config_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/') {
        n = snprintf(out, cap, "%s/dragongl/client.conf", xdg);
    } else if (home && home[0]) {
        n = snprintf(out, cap, "%s/.config/dragongl/client.conf", home);
    } else {
        return false;
    }
    return n > 0 && (size_t)n < cap;
}

int client_config_load(ClientConfig *cfg, ConfigWarn warn, void *user) {
    client_config_defaults(cfg);
    char path[512];
    if (!client_config_path(path, sizeof(path))) {
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; /*no file: the defaults*/
    }
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    bool whole = feof(f) && !ferror(f);
    fclose(f);
    if (!whole) {
        return -1;
    }
    text[n] = '\0';
    return client_config_parse(cfg, text, warn, user);
}

void client_config_print(const ClientConfig *cfg, FILE *out) {
    char a[40], b[40];
    fprintf(out, "# Dragon GL client settings. Lines starting with '#' are comments.\n\n");
    fprintf(out, "# Size of the HUD: auto (from the monitor and the window), or 0.5 to 4.\n");
    if (cfg->ui_scale > 0.0f) {
        fprintf(out, "ui_scale = %.2f\n\n", (double)cfg->ui_scale);
    } else {
        fprintf(out, "ui_scale = auto\n\n");
    }
    fprintf(out, "# yes: no particle effects, combat numbers do not drift.\n");
    fprintf(out, "reduce_motion = %s\n\n", cfg->reduce_motion ? "yes" : "no");
    fprintf(out, "# yes: frame time in the top right corner.\n");
    fprintf(out, "show_fps = %s\n\n", cfg->show_fps ? "yes" : "no");
    fprintf(out,
            "# Keys: up to two per action, \"none\" for no key. Modifiers: Shift+ Ctrl+ Alt+.\n");
    for (int act = 0; act < ACT_COUNT; act++) {
        key_chord_format(cfg->keys[act][0], a, sizeof(a));
        key_chord_format(cfg->keys[act][1], b, sizeof(b));
        char value[96];
        if (cfg->keys[act][1].key != 0) {
            snprintf(value, sizeof(value), "%s, %s", a, b);
        } else {
            snprintf(value, sizeof(value), "%s", a);
        }
        fprintf(out, "key.%-13s = %-28s # %s\n", k_actions[act].name, value, k_actions[act].label);
    }
    fprintf(out, "\n# A key that sends a command, as if typed:\n");
    if (cfg->macro_count == 0) {
        fprintf(out, "# bind.F5 = look\n# bind.I = inventory\n");
    }
    for (int i = 0; i < cfg->macro_count; i++) {
        key_chord_format(cfg->macros[i].chord, a, sizeof(a));
        fprintf(out, "bind.%s = %s\n", a, cfg->macros[i].command);
    }
}
