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
#include "lachesis_scale.h"

#include <libplacebo/filters.h>

static const struct pl_filter_config *chosen = NULL;

static struct pl_filter_config haasnsoft;

static void ensure_haasnsoft(void) {
    if (haasnsoft.name) {
        return;
    }
    haasnsoft = pl_filter_ewa_hann;
    haasnsoft.name = "haasnsoft";
    haasnsoft.description = "Soft EWA Hann";
    haasnsoft.blur = 1.11f;
}

static int filter_count(void) {
    return pl_num_filter_configs + 1;
}

static const struct pl_filter_config *filter_at(int i) {
    return i < pl_num_filter_configs ? pl_filter_configs[i] : &haasnsoft;
}

/* Frame mixing filters live in the same list but are no use to us here. */
static int is_scaler(const struct pl_filter_config *config) {
    return config && (config->allowed & PL_FILTER_SCALING) != 0;
}

const struct pl_filter_config *scale_filter(void) {
    return chosen;
}

int scale_filter_set(const char *name) {
    ensure_haasnsoft();

    for (int i = 0; i < filter_count(); i++) {
        const struct pl_filter_config *config = filter_at(i);

        if (is_scaler(config) && !strcmp(name, config->name)) {
            chosen = config;
            return 1;
        }
    }

    return 0;
}

static void format_usage(const struct pl_filter_config *config, char *buf,
                         size_t size) {
    int up = (config->allowed & PL_FILTER_UPSCALING) != 0;
    int down = (config->allowed & PL_FILTER_DOWNSCALING) != 0;

    snprintf(buf, size, "%s%s%s%s%s", up ? "up" : "",
             up && (config->recommended & PL_FILTER_UPSCALING) ? "*" : "",
             up && down ? ", " : "", down ? "down" : "",
             down && (config->recommended & PL_FILTER_DOWNSCALING) ? "*" : "");
}

static const char *describe(const struct pl_filter_config *config) {
    return config->description ? config->description : "alias";
}

void scale_filter_list(void) {
    int name_width = 0, desc_width = 0;

    ensure_haasnsoft();

    for (int i = 0; i < filter_count(); i++) {
        const struct pl_filter_config *config = filter_at(i);
        int name_len, desc_len;

        if (!is_scaler(config)) {
            continue;
        }
        name_len = (int)strlen(config->name);
        desc_len = (int)strlen(describe(config));
        if (name_len > name_width) {
            name_width = name_len;
        }
        if (desc_len > desc_width) {
            desc_width = desc_len;
        }
    }

    printf("Available scalers:\n");
    for (int i = 0; i < filter_count(); i++) {
        const struct pl_filter_config *config = filter_at(i);
        char usage[32];

        if (!is_scaler(config)) {
            continue;
        }
        format_usage(config, usage, sizeof(usage));
        printf("  %-*s  %-*s  %s\n", name_width, config->name, desc_width,
               describe(config), usage);
    }
}
