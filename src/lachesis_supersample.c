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
 *
 * Based on FidelityFX RCAS:
 * Copyright © 2023 Advanced Micro Devices, Inc.
 */

#include <stdio.h>
#include <string.h>

#include "lachesis_config.h"
#include "lachesis_supersample.h"

#include <libplacebo/filters.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>

enum SupersampleLevel supersample_level = SUPERSAMPLE_OFF;

/* clang-format off */
static const char supersample_shader[] =
    "//!PARAM strength\n"
    "//!DESC Sharpening strength\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "0.0\n"
    "\n"
    "//!HOOK MAIN\n"
    "//!BIND HOOKED\n"
    "//!DESC Contrast-adaptive sharpening\n"
    "//!WHEN strength 0.0 >\n"
    "\n"
    "#define LOBE_LIMIT 0.1875\n"
    "#define EPSILON 1e-5\n"
    "\n"
    "float ss_luma(vec3 c) {\n"
    "    return c.g + 0.5 * (c.r + c.b);\n"
    "}\n"
    "\n"
    "vec4 hook() {\n"
    "    vec4 e = HOOKED_texOff(vec2( 0.0,  0.0));\n"
    "    vec3 b = HOOKED_texOff(vec2( 0.0, -1.0)).rgb;\n"
    "    vec3 d = HOOKED_texOff(vec2(-1.0,  0.0)).rgb;\n"
    "    vec3 f = HOOKED_texOff(vec2( 1.0,  0.0)).rgb;\n"
    "    vec3 h = HOOKED_texOff(vec2( 0.0,  1.0)).rgb;\n"
    "\n"
    "    vec3 cb = clamp(b, 0.0, 1.0);\n"
    "    vec3 cd = clamp(d, 0.0, 1.0);\n"
    "    vec3 cf = clamp(f, 0.0, 1.0);\n"
    "    vec3 ch = clamp(h, 0.0, 1.0);\n"
    "    vec3 ce = clamp(e.rgb, 0.0, 1.0);\n"
    "\n"
    "    vec3 mn = min(min(cb, cd), min(cf, ch));\n"
    "    vec3 mx = max(max(cb, cd), max(cf, ch));\n"
    "\n"
    "    vec3 hit_min = mn / max(4.0 * mx, EPSILON);\n"
    "    vec3 hit_max = (1.0 - mx) / min(4.0 * mn - 4.0, -EPSILON);\n"
    "    vec3 lobe_rgb = max(-hit_min, hit_max);\n"
    "    float lobe = max(-LOBE_LIMIT,\n"
    "                     min(max(max(lobe_rgb.r, lobe_rgb.g), lobe_rgb.b), 0.0));\n"
    "\n"
    "    float bl = ss_luma(cb);\n"
    "    float dl = ss_luma(cd);\n"
    "    float el = ss_luma(ce);\n"
    "    float fl = ss_luma(cf);\n"
    "    float hl = ss_luma(ch);\n"
    "    float span = max(max(max(bl, dl), max(fl, hl)), el) -\n"
    "                 min(min(min(bl, dl), min(fl, hl)), el);\n"
    "    float noise = clamp(abs(0.25 * (bl + dl + fl + hl) - el) /\n"
    "                        max(span, EPSILON), 0.0, 1.0);\n"
    "\n"
    "    lobe *= (1.0 - 0.5 * noise) * strength;\n"
    "\n"
    "    return vec4((e.rgb + lobe * (b + d + f + h)) / (1.0 + 4.0 * lobe), e.a);\n"
    "}\n";
/* clang-format on */

static float supersample_strength(enum SupersampleLevel level) {
    switch (level) {
    case SUPERSAMPLE_LIGHT:
        return 0.40f;
    case SUPERSAMPLE_MEDIUM:
        return 0.65f;
    case SUPERSAMPLE_STRONG:
        return 0.90f;
    default:
        return 0.0f;
    }
}

enum SupersampleLevel supersample_level_parse(const char *name) {
    static const enum SupersampleLevel levels[] = {
        SUPERSAMPLE_OFF,
        SUPERSAMPLE_LIGHT,
        SUPERSAMPLE_MEDIUM,
        SUPERSAMPLE_STRONG,
    };

    if (!name) {
        return SUPERSAMPLE_OFF;
    }
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (!strcmp(name, supersample_level_name(levels[i]))) {
            return levels[i];
        }
    }

    return SUPERSAMPLE_OFF;
}

const char *supersample_status(enum SupersampleLevel level,
                               enum SupersampleState state) {
    static char status[96];
    const char *name = supersample_level_name(level);
    const char *reason = NULL;

    if (level == SUPERSAMPLE_OFF) {
        return name;
    }

    switch (state) {
    case SUPERSAMPLE_NO_RENDERER:
        reason = "no renderer to run it";
        break;
    case SUPERSAMPLE_BENCHMARKING:
        reason = "suspended while benchmarking";
        break;
    case SUPERSAMPLE_DEGRADED:
        reason = "suspended while decoding is degraded";
        break;
    default:
        break;
    }

    if (reason) {
        snprintf(status, sizeof(status), "%s, %s", name, reason);
    } else {
        snprintf(status, sizeof(status), "%s, RCAS %.2f", name,
                 supersample_strength(level));
    }

    return status;
}

const struct pl_hook *supersample_pl_hook_create(const struct pl_gpu_t *gpu) {
    return pl_mpv_user_shader_parse(gpu, supersample_shader,
                                    sizeof(supersample_shader) - 1);
}

void supersample_pl_hook_destroy(const struct pl_hook **hook) {
    pl_mpv_user_shader_destroy(hook);
}

void supersample_pl_hook_update(const struct pl_hook *hook,
                                enum SupersampleLevel level) {
    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *par = &hook->parameters[i];

        if (!strcmp(par->name, "strength")) {
            par->data->f = supersample_strength(level);
        }
    }
}

const struct pl_deband_params *
supersample_deband_params(enum SupersampleLevel level) {
    static const struct pl_deband_params medium = {
        .iterations = 1,
        .threshold = 3.0f,
        .radius = 16.0f,
        .grain = 4.0f,
    };
    static const struct pl_deband_params strong = {
        .iterations = 2,
        .threshold = 4.0f,
        .radius = 16.0f,
        .grain = 6.0f,
    };

    switch (level) {
    case SUPERSAMPLE_MEDIUM:
        return &medium;
    case SUPERSAMPLE_STRONG:
        return &strong;
    default:
        return NULL;
    }
}

const struct pl_filter_config *
supersample_upscaler(enum SupersampleLevel level) {
    return level == SUPERSAMPLE_STRONG ? &pl_filter_ewa_lanczossharp : NULL;
}
