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
 * client_math.c - Matrices and vectors of the renderer (client_math.h).
 */

#include "client_math.h"

#include <math.h>

void mat4_mul(float c[16], const float a[16], const float b[16]) {
    float tmp[16];
    int col, row, k, i;
    for (i = 0; i < 16; i++) {
        tmp[i] = 0.0f;
    }
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            for (k = 0; k < 4; k++) {
                tmp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    for (i = 0; i < 16; i++) {
        c[i] = tmp[i];
    }
}

void mat4_perspective(float m[16], float fov_rad, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fov_rad * 0.5f);
    float A = zfar / (znear - zfar);
    float B = (znear * zfar) / (znear - zfar);
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = 0.0f;
    }
    m[0 * 4 + 0] = f / aspect;
    m[1 * 4 + 1] = -f;
    m[2 * 4 + 2] = A;
    m[2 * 4 + 3] = -1.0f;
    m[3 * 4 + 2] = B;
}

/*2D orthographic projection: X into [0..w], Y into [0..h], Z into [-1..1]*/
void mat4_ortho(float m[16], float w, float h) {
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = 0.0f;
    }
    m[0 * 4 + 0] = 2.0f / w;
    m[1 * 4 + 1] = 2.0f / h;
    m[2 * 4 + 2] = -1.0f;
    m[3 * 4 + 0] = -1.0f;
    m[3 * 4 + 1] = -1.0f;
    m[3 * 4 + 3] = 1.0f;
}

void vec3_normalize(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

void vec3_cross(float r[3], const float a[3], const float b[3]) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

float vec3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void mat4_lookat(float m[16], const float eye[3], const float center[3], const float world_up[3]) {
    float f[3], s[3], u[3];
    int i;
    for (i = 0; i < 3; i++) {
        f[i] = center[i] - eye[i];
    }
    vec3_normalize(f);
    vec3_cross(s, f, world_up);
    vec3_normalize(s);
    vec3_cross(u, s, f);

    for (i = 0; i < 16; i++) {
        m[i] = 0.0f;
    }
    m[0 * 4 + 0] = s[0];
    m[1 * 4 + 0] = s[1];
    m[2 * 4 + 0] = s[2];
    m[0 * 4 + 1] = u[0];
    m[1 * 4 + 1] = u[1];
    m[2 * 4 + 1] = u[2];
    m[0 * 4 + 2] = -f[0];
    m[1 * 4 + 2] = -f[1];
    m[2 * 4 + 2] = -f[2];
    m[3 * 4 + 0] = -vec3_dot(s, eye);
    m[3 * 4 + 1] = -vec3_dot(u, eye);
    m[3 * 4 + 2] = vec3_dot(f, eye);
    m[3 * 4 + 3] = 1.0f;
}

void project_point(const float mvp[16], float x, float y, float z, float sw, float sh, float *sx,
                   float *sy, bool *visible) {
    float clip_x = x * mvp[0] + y * mvp[4] + z * mvp[8] + mvp[12];
    float clip_y = x * mvp[1] + y * mvp[5] + z * mvp[9] + mvp[13];
    float clip_w = x * mvp[3] + y * mvp[7] + z * mvp[11] + mvp[15];

    if (clip_w <= 0.1f) {
        *visible = false;
        return;
    }

    float ndc_x = clip_x / clip_w;
    float ndc_y = clip_y / clip_w;

    *sx = (ndc_x + 1.0f) * 0.5f * sw;
    *sy = (ndc_y + 1.0f) * 0.5f * sh;

    *visible = (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f);
}
