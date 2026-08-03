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

#include "lachesis_config.h"

#include <math.h>

#include <libavutil/common.h>

#include "lachesis_equalizer.h"
#include "lachesis_osd.h"

static EqualizerValues eq_values;

EqualizerValues equalizer_get(void) {
    return eq_values;
}

float equalizer_pl_brightness(int brightness) {
    return brightness / 100.0f;
}

float equalizer_pl_contrast(int contrast) {
    return (contrast + 100) / 100.0f;
}

float equalizer_pl_gamma(int gamma) {
    return (float)exp(log(8.0) * gamma / 100.0);
}

void equalizer_adjust(enum EqualizerControl control, int steps) {
    const char *name;
    int *value;

    switch (control) {
    case EQ_BRIGHTNESS:
        name = "Brightness";
        value = &eq_values.brightness;
        break;
    case EQ_GAMMA:
        name = "Gamma";
        value = &eq_values.gamma;
        break;
    case EQ_CONTRAST:
        name = "Contrast";
        value = &eq_values.contrast;
        break;
    default:
        return;
    }

    *value = av_clip(*value + steps, EQ_VALUE_MIN, EQ_VALUE_MAX);
    osd_show_message("%s: %d", name, *value);
}
