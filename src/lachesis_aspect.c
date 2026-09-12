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

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/macros.h>
#include <libavutil/rational.h>

#include "lachesis_aspect.h"

#define ASPECT_MIN 0.05
#define ASPECT_MAX 20.0
#define ASPECT_DEN_MAX 10000
#define ASPECT_SOURCE_NAME "source"
#define ASPECT_SQUARE_NAME "square pixels"

enum AspectMode {
    ASPECT_FROM_FILE,
    ASPECT_SQUARE_PIXELS,
    ASPECT_FIXED,
};

typedef struct AspectChoice {
    const char *name;
    enum AspectMode mode;
    AVRational dar;
} AspectChoice;

static const AspectChoice aspect_choices[] = {
    {ASPECT_SOURCE_NAME, ASPECT_FROM_FILE, {0, 1}},
    {ASPECT_SQUARE_NAME, ASPECT_SQUARE_PIXELS, {0, 1}},
    {"4:3", ASPECT_FIXED, {4, 3}},
    {"16:10", ASPECT_FIXED, {16, 10}},
    {"16:9", ASPECT_FIXED, {16, 9}},
    {"1.85:1", ASPECT_FIXED, {37, 20}},
    {"2.35:1", ASPECT_FIXED, {47, 20}},
};

static enum AspectMode aspect_mode = ASPECT_FROM_FILE;
static AVRational aspect_dar = {0, 1};
static char aspect_name[32] = ASPECT_SOURCE_NAME;

static void aspect_take(const AspectChoice *choice) {
    aspect_mode = choice->mode;
    aspect_dar = choice->dar;
    snprintf(aspect_name, sizeof(aspect_name), "%s", choice->name);
}

static const AspectChoice *aspect_choice_for(enum AspectMode mode) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(aspect_choices); i++) {
        if (aspect_choices[i].mode == mode) {
            return &aspect_choices[i];
        }
    }

    return &aspect_choices[0];
}

static int aspect_choice_index(void) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(aspect_choices); i++) {
        const AspectChoice *choice = &aspect_choices[i];

        if (choice->mode != aspect_mode) {
            continue;
        }
        if (aspect_mode != ASPECT_FIXED ||
            !av_cmp_q(choice->dar, aspect_dar)) {
            return (int)i;
        }
    }

    return -1;
}

static int parse_side(const char *at, char **end, double *out) {
    errno = 0;
    *out = strtod(at, end);

    return !errno && *end != at && isfinite(*out) && *out > 0.0;
}

static int parse_ratio(const char *arg, AVRational *out) {
    double num, den = 1.0, value;
    char *end = NULL;

    if (!parse_side(arg, &end, &num)) {
        return 0;
    }
    if (*end == ':' || *end == '/') {
        if (!parse_side(end + 1, &end, &den)) {
            return 0;
        }
    }
    if (*end) {
        return 0;
    }

    value = num / den;
    if (!(value >= ASPECT_MIN) || !(value <= ASPECT_MAX)) {
        return 0;
    }
    *out = av_d2q(value, ASPECT_DEN_MAX);

    return out->num > 0 && out->den > 0;
}

int aspect_override_set(const char *arg) {
    AVRational dar;

    if (!arg || !arg[0]) {
        return 0;
    }
    if (!strcmp(arg, "off") || !strcmp(arg, "no") || !strcmp(arg, "none") ||
        !strcmp(arg, ASPECT_SOURCE_NAME)) {
        aspect_take(aspect_choice_for(ASPECT_FROM_FILE));
        return 1;
    }
    if (!strcmp(arg, "square")) {
        aspect_take(aspect_choice_for(ASPECT_SQUARE_PIXELS));
        return 1;
    }
    if (!parse_ratio(arg, &dar)) {
        return 0;
    }
    aspect_mode = ASPECT_FIXED;
    aspect_dar = dar;
    snprintf(aspect_name, sizeof(aspect_name), "%s", arg);

    return 1;
}

const char *aspect_override_cycle(void) {
    int at = aspect_choice_index();

    aspect_take(&aspect_choices[(at + 1) % (int)FF_ARRAY_ELEMS(aspect_choices)]);

    return aspect_name;
}

const char *aspect_override_label(void) {
    return aspect_name;
}

int aspect_override_active(void) {
    return aspect_mode != ASPECT_FROM_FILE;
}

AVRational aspect_override_sar(int pic_width, int pic_height, AVRational sar) {
    AVRational want;

    if (aspect_mode == ASPECT_FROM_FILE) {
        return sar;
    }
    if (aspect_mode == ASPECT_SQUARE_PIXELS || pic_width < 1 || pic_height < 1) {
        return (AVRational){1, 1};
    }

    want = av_div_q(aspect_dar, av_make_q(pic_width, pic_height));
    if (want.num <= 0 || want.den <= 0) {
        return (AVRational){1, 1};
    }

    return want;
}
