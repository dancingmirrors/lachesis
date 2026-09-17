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

#ifndef LACHESIS_SUPERSAMPLE_H
#define LACHESIS_SUPERSAMPLE_H

enum SupersampleLevel {
    SUPERSAMPLE_OFF,
    SUPERSAMPLE_LIGHT,
    SUPERSAMPLE_MEDIUM,
    SUPERSAMPLE_STRONG,
};

enum SupersampleState {
    SUPERSAMPLE_RUNNING,
    SUPERSAMPLE_NO_RENDERER,
    SUPERSAMPLE_BENCHMARKING,
};

static inline enum SupersampleLevel
supersample_level_next(enum SupersampleLevel level) {
    switch (level) {
    case SUPERSAMPLE_OFF:
        return SUPERSAMPLE_LIGHT;
    case SUPERSAMPLE_LIGHT:
        return SUPERSAMPLE_MEDIUM;
    case SUPERSAMPLE_MEDIUM:
        return SUPERSAMPLE_STRONG;
    default:
        return SUPERSAMPLE_OFF;
    }
}

extern const char *const supersample_level_names[];

static inline const char *supersample_level_name(enum SupersampleLevel level) {
    switch (level) {
    case SUPERSAMPLE_LIGHT:
    case SUPERSAMPLE_MEDIUM:
    case SUPERSAMPLE_STRONG:
        return supersample_level_names[level];
    default:
        return supersample_level_names[SUPERSAMPLE_OFF];
    }
}

enum SupersampleLevel supersample_level_parse(const char *name);

const char *supersample_status(enum SupersampleLevel level,
                               enum SupersampleState state);

struct pl_gpu_t;
struct pl_hook;
struct pl_deband_params;
struct pl_filter_config;

const struct pl_hook *supersample_pl_hook_create(const struct pl_gpu_t *gpu);

void supersample_pl_hook_destroy(const struct pl_hook **hook);

void supersample_pl_hook_update(const struct pl_hook *hook,
                                enum SupersampleLevel level);

const struct pl_deband_params *
supersample_deband_params(enum SupersampleLevel level);

const struct pl_filter_config *
supersample_upscaler(enum SupersampleLevel level);

#endif /* LACHESIS_SUPERSAMPLE_H */
