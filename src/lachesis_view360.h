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

#ifndef LACHESIS_VIEW360_H
#define LACHESIS_VIEW360_H

enum View360Layout {
    VIEW360_LAYOUT_OFF,
    VIEW360_LAYOUT_FULL,
    VIEW360_LAYOUT_TB,
};

static inline float view360_default_yaw(enum View360Layout layout) {
    return layout == VIEW360_LAYOUT_FULL ? 90.0f : 0.0f;
}

static inline enum View360Layout view360_layout_next(enum View360Layout layout) {
    switch (layout) {
    case VIEW360_LAYOUT_OFF:
        return VIEW360_LAYOUT_FULL;
    case VIEW360_LAYOUT_FULL:
        return VIEW360_LAYOUT_TB;
    default:
        return VIEW360_LAYOUT_OFF;
    }
}

static inline const char *view360_layout_name(enum View360Layout layout) {
    switch (layout) {
    case VIEW360_LAYOUT_FULL:
        return "side-by-side";
    case VIEW360_LAYOUT_TB:
        return "top-bottom";
    default:
        return "off";
    }
}

#define VIEW360_DEFAULT_HFOV 140.0f
#define VIEW360_DEFAULT_PITCH -15.0f

typedef struct View360Viewport {
    float off_x, off_y;
    float scale_x, scale_y;
    float aspect;
} View360Viewport;

#define VIEW360_VIEWPORT_WHOLE \
    (View360Viewport){.scale_x = 1.0f, .scale_y = 1.0f, .aspect = 1.0f}

struct pl_gpu_t;
struct pl_hook;

const struct pl_hook *view360_pl_hook_create(const struct pl_gpu_t *gpu);

void view360_pl_hook_destroy(const struct pl_hook **hook);

void view360_pl_hook_update(const struct pl_hook *hook, float yaw, float pitch,
                            float roll, float hfov, enum View360Layout layout,
                            int rotate, const View360Viewport *viewport);

#endif /* LACHESIS_VIEW360_H */
