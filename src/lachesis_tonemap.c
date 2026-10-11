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

#include <stdio.h>
#include <string.h>

#include "lachesis_config.h"
#include "lachesis_tonemap.h"

#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/tone_mapping.h>

static const struct pl_tone_map_function *chosen = NULL;

const struct pl_tone_map_function *tone_map_function(void) {
    return chosen;
}

static int same_name(const char *arg, const char *name) {
    for (; *arg || *name; arg++) {
        if (*arg == '.') {
            continue;
        }
        if (*arg != *name++) {
            return 0;
        }
    }

    return 1;
}

int tone_map_set(const char *name) {
    if (!strcmp(name, "auto")) {
        chosen = NULL;
        return 1;
    }
    for (int i = 0; i < pl_num_tone_map_functions; i++) {
        if (same_name(name, pl_tone_map_functions[i]->name)) {
            chosen = pl_tone_map_functions[i];
            return 1;
        }
    }

    return 0;
}

void tone_map_list(void) {
    const struct pl_tone_map_function *fallback =
        pl_color_map_default_params.tone_mapping_function;
    int width = (int)strlen("auto");

    for (int i = 0; i < pl_num_tone_map_functions; i++) {
        int len = (int)strlen(pl_tone_map_functions[i]->name);

        if (len > width) {
            width = len;
        }
    }

    printf("Available tone mapping curves:\n");
    printf("  %-*s  the default, currently %s\n", width, "auto",
           fallback ? fallback->name : "clip");
    for (int i = 0; i < pl_num_tone_map_functions; i++) {
        const struct pl_tone_map_function *fn = pl_tone_map_functions[i];

        printf("  %-*s  %s\n", width, fn->name,
               fn->description ? fn->description : "");
    }
}
