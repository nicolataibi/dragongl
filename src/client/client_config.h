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
 * client_config.h - The settings of the client and its keys.
 *
 * Read from $XDG_CONFIG_HOME/dragongl/client.conf (else
 * ~/.config/dragongl/client.conf), one "name = value" per line, '#' starts
 * a comment:
 *
 *   ui_scale = auto            size of the HUD: auto, or 0.5 to 4
 *   reduce_motion = no         yes: no particles, no drifting combat text
 *   show_fps = no              yes: frame time in the HUD
 *   key.move_north = W, Up     up to two keys per action; "none" unbinds
 *   bind.F5 = look             a key that sends a command
 *
 * A key is a name (A..Z, 0..9, F1..F12, Up, Enter, PageUp, Minus, KP8,
 * ...) with optional Shift+, Ctrl+ and Alt+ in front. "dragongl_client
 * --print-config" writes the file with every default. No rendering and no
 * input library here (tests/test_client_ui.c): the key codes are those of
 * GLFW, checked where GLFW is included (client_input.c).
 */

#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    ACT_NONE = -1,
    ACT_MOVE_NORTH,
    ACT_MOVE_SOUTH,
    ACT_MOVE_WEST,
    ACT_MOVE_EAST,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_CAMERA_LEFT, /*turn the camera around the character*/
    ACT_CAMERA_RIGHT,
    ACT_CAMERA_UP, /*look from higher up*/
    ACT_CAMERA_DOWN,
    ACT_COMMAND, /*open the command line*/
    ACT_LOG_UP,  /*scroll the messages back*/
    ACT_LOG_DOWN,
    ACT_SCALE_UP, /*larger HUD*/
    ACT_SCALE_DOWN,
    ACT_TOGGLE_FPS,
    ACT_TOGGLE_PANELS, /*show or hide equipment and trophies*/
    ACT_HELP,          /*show or hide the list of keys*/
    ACT_QUIT,
    ACT_COUNT
} ClientAction;

/*Modifier bits, as in GLFW.*/
#define KEYMOD_SHIFT 0x1
#define KEYMOD_CTRL  0x2
#define KEYMOD_ALT   0x4
#define KEYMOD_MASK  0x7

typedef struct {
    int key; /*0: not bound*/
    int mods;
} KeyChord;

#define CONFIG_KEYS_PER_ACTION 2
#define CONFIG_MAX_MACROS      32
#define CONFIG_MACRO_LEN       128

typedef struct {
    KeyChord chord;
    char command[CONFIG_MACRO_LEN];
} KeyMacro;

#define UI_SCALE_MIN 0.5f
#define UI_SCALE_MAX 4.0f

typedef struct {
    float ui_scale; /*0: automatic, from the monitor and the window*/
    bool reduce_motion;
    bool show_fps;
    KeyChord keys[ACT_COUNT][CONFIG_KEYS_PER_ACTION];
    KeyMacro macros[CONFIG_MAX_MACROS];
    int macro_count;
} ClientConfig;

/*Called for every line that is not understood (the setting keeps its
 * value).*/
typedef void (*ConfigWarn)(int line, const char *message, void *user);

void client_config_defaults(ClientConfig *cfg);
/*Applies the text of a configuration file over cfg. Returns the number of
 * lines refused.*/
int client_config_parse(ClientConfig *cfg, const char *text, ConfigWarn warn, void *user);
/*Path of the configuration file; false without $XDG_CONFIG_HOME and
 * $HOME.*/
bool client_config_path(char *out, size_t cap);
/*Defaults, then the file if there is one. Returns the lines refused, -1
 * if the file exists and cannot be read.*/
int client_config_load(ClientConfig *cfg, ConfigWarn warn, void *user);
/*The configuration as a file, comments included.*/
void client_config_print(const ClientConfig *cfg, FILE *out);

/*What a key does: an action, or ACT_NONE.*/
ClientAction client_config_action(const ClientConfig *cfg, int key, int mods);
/*The command bound to a key, or NULL.*/
const char *client_config_macro(const ClientConfig *cfg, int key, int mods);

/*"Ctrl+Equal" <-> chord. key_chord_parse() is false for an unknown name;
 * "none" gives key 0.*/
bool key_chord_parse(const char *s, KeyChord *out);
void key_chord_format(KeyChord chord, char *out, size_t cap);

/*"move_north", and what it does in a few words ("Step north").*/
const char *client_action_name(ClientAction a);
const char *client_action_label(ClientAction a);

/*The automatic HUD scale: the height of the framebuffer over 800 pixels
 * (the HUD keeps its share of the window), never under 0.75 times the
 * content scale of the monitor (1 on an ordinary one, 2 on a HiDPI one);
 * in steps of 0.25 within UI_SCALE_MIN..UI_SCALE_MAX.*/
float ui_scale_auto(float content_scale, int framebuffer_height);
/*A scale typed by the user, brought to a step of 0.25 within the range.*/
float ui_scale_clamp(float scale);

#endif /* CLIENT_CONFIG_H */
