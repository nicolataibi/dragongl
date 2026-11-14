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
 * client_input.h - Keyboard and mouse of the window: the camera, the
 * actions of the key map (client_config.h), the command line and what the
 * HUD shows because of them. Everything here runs on the render thread
 * (GLFW callbacks and the HUD), so there is no lock.
 */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include "client_config.h"
#include "line_edit.h"

#include <GLFW/glfw3.h>

/*The camera orbits the character: angles in degrees, distance in tiles.*/
typedef struct {
    float yaw, pitch, dist;
} Camera;

typedef struct {
    bool typing;          /*the command line is open*/
    const LineEdit *line; /*its text and cursor*/
    int log_scroll;       /*rows of the message panel scrolled back*/
    bool show_help;       /*the list of keys*/
    bool show_fps;
    bool show_side; /*equipment and trophies*/
    float scale;    /*HUD scale in use*/
    bool reduce_motion;
} ClientUi;

/*Installs the callbacks on the window. cfg must outlive the window.*/
void client_input_install(GLFWwindow *window, const ClientConfig *cfg);
const Camera *client_camera(void);
const ClientUi *client_ui(void);
const ClientConfig *client_input_config(void);
/*Recomputes the automatic HUD scale and which panels fit (new
 * framebuffer size or monitor).*/
void client_input_resized(GLFWwindow *window, int framebuffer_width, int framebuffer_height);

#endif /* CLIENT_INPUT_H */
