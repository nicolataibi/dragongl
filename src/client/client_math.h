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
 * client_math.h - The matrices and vectors the renderer needs: 4x4
 * column-major matrices (the layout of the shaders' push constants) and
 * 3-component vectors.
 */

#ifndef CLIENT_MATH_H
#define CLIENT_MATH_H

#include <stdbool.h>

/*c = a * b.*/
void mat4_mul(float c[16], const float a[16], const float b[16]);
void mat4_perspective(float m[16], float fov_rad, float aspect, float znear, float zfar);
/*Pixel coordinates of a w x h surface, origin at the top left.*/
void mat4_ortho(float m[16], float w, float h);
void mat4_lookat(float m[16], const float eye[3], const float center[3], const float world_up[3]);
void vec3_normalize(float v[3]);
void vec3_cross(float r[3], const float a[3], const float b[3]);
float vec3_dot(const float a[3], const float b[3]);
/*Where a point of the world falls on a sw x sh surface. visible is false
 * behind the camera or outside the surface.*/
void project_point(const float mvp[16], float x, float y, float z, float sw, float sh, float *sx,
                   float *sy, bool *visible);

#endif /* CLIENT_MATH_H */
