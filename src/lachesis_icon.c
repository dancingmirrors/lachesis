/*
 * Copyright © 2026 dancingmirrors@icloud.com
 *
 * This file is part of lachesis.
 *
 * lachesis is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * lachesis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with lachesis; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <math.h>

#include "lachesis_icon.h"

#define ICON_VIEWBOX 256.0f
#define FRAME_X 18.0f
#define FRAME_Y 18.0f
#define FRAME_SIZE 220.0f
#define FRAME_RADIUS 46.0f
#define FRAME_STROKE 4.0f
#define GLYPH_STROKE 12.0f

static const float glyph[3][2] = {{108, 86}, {108, 170}, {180, 128}};

static const float frame_top[3] = {0x30 / 255.0f, 0x30 / 255.0f, 0x30 / 255.0f};
static const float frame_bottom[3] = {0x08 / 255.0f, 0x08 / 255.0f, 0x08 / 255.0f};
static const float frame_edge[3] = {0x56 / 255.0f, 0x56 / 255.0f, 0x56 / 255.0f};
static const float glyph_color[3] = {0xf4 / 255.0f, 0xf4 / 255.0f, 0xf4 / 255.0f};

static float rounded_box_sdf(float px, float py) {
    float hw = FRAME_SIZE / 2 - FRAME_RADIUS;
    float hh = FRAME_SIZE / 2 - FRAME_RADIUS;
    float dx = fabsf(px - (FRAME_X + FRAME_SIZE / 2)) - hw;
    float dy = fabsf(py - (FRAME_Y + FRAME_SIZE / 2)) - hh;
    float ox = dx > 0 ? dx : 0;
    float oy = dy > 0 ? dy : 0;
    float inner = dx > dy ? dx : dy;

    return sqrtf(ox * ox + oy * oy) + (inner < 0 ? inner : 0) - FRAME_RADIUS;
}

static float segment_distance(float px, float py, const float a[2],
                              const float b[2]) {
    float vx = b[0] - a[0];
    float vy = b[1] - a[1];
    float wx = px - a[0];
    float wy = py - a[1];
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? (wx * vx + wy * vy) / len2 : 0;

    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    wx -= vx * t;
    wy -= vy * t;

    return sqrtf(wx * wx + wy * wy);
}

static float triangle_sdf(float px, float py) {
    float best = segment_distance(px, py, glyph[0], glyph[1]);
    int inside = 0;

    for (int i = 0; i < 3; i++) {
        const float *a = glyph[i];
        const float *b = glyph[(i + 1) % 3];
        float d = segment_distance(px, py, a, b);

        if (d < best) {
            best = d;
        }
        if ((a[1] > py) != (b[1] > py) &&
            px < a[0] + (py - a[1]) / (b[1] - a[1]) * (b[0] - a[0])) {
            inside = !inside;
        }
    }

    return inside ? -best : best;
}

static float coverage(float distance, float scale) {
    float half = scale * 0.5f;

    if (distance <= -half) {
        return 1.0f;
    }
    if (distance >= half) {
        return 0.0f;
    }
    return 0.5f - distance / scale;
}

static void blend(float dst[4], const float src[3], float alpha) {
    if (alpha <= 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        dst[i] = dst[i] * (1 - alpha) + src[i] * alpha;
    }
    dst[3] = dst[3] * (1 - alpha) + alpha;
}

static SDL_Surface *render_icon(int size) {
    SDL_Surface *surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    float scale = ICON_VIEWBOX / size;
    uint8_t *pixels;

    if (!surface) {
        return NULL;
    }

    pixels = surface->pixels;
    for (int y = 0; y < size; y++) {
        uint8_t *row = pixels + (size_t)y * surface->pitch;

        for (int x = 0; x < size; x++) {
            float px = (x + 0.5f) * scale;
            float py = (y + 0.5f) * scale;
            float rgba[4] = {0, 0, 0, 0};
            float box = rounded_box_sdf(px, py);
            float tri = triangle_sdf(px, py) - GLYPH_STROKE / 2;
            float t = (py - FRAME_Y) / FRAME_SIZE;
            float fill[3];

            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            for (int i = 0; i < 3; i++) {
                fill[i] = frame_top[i] * (1 - t) + frame_bottom[i] * t;
            }

            blend(rgba, fill, coverage(box, scale));
            blend(rgba, frame_edge,
                  coverage(fabsf(box) - FRAME_STROKE / 2, scale));
            blend(rgba, glyph_color, coverage(tri, scale));

            for (int i = 0; i < 4; i++) {
                float v = rgba[i] * 255.0f + 0.5f;

                row[x * 4 + i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }

    return surface;
}

void icon_set_window_icon(SDL_Window *window) {
    static const int sizes[] = {32, 64, 128, 256};
    SDL_Surface *base;

    if (!window) {
        return;
    }

    base = render_icon(sizes[0]);
    if (!base) {
        return;
    }

    for (size_t i = 1; i < SDL_arraysize(sizes); i++) {
        SDL_Surface *alt = render_icon(sizes[i]);

        if (alt) {
            SDL_AddSurfaceAlternateImage(base, alt);
            SDL_DestroySurface(alt);
        }
    }

    SDL_SetWindowIcon(window, base);
    SDL_DestroySurface(base);
}
