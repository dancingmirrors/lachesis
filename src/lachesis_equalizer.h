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

#ifndef LACHESIS_EQUALIZER_H
#define LACHESIS_EQUALIZER_H

#include <libavutil/frame.h>

enum EqualizerControl {
    EQ_BRIGHTNESS,
    EQ_GAMMA,
    EQ_CONTRAST,
    EQ_CONTROL_COUNT,
};

#define EQ_VALUE_MIN (-100)
#define EQ_VALUE_MAX 100

typedef struct EqualizerValues {
    int brightness;
    int gamma;
    int contrast;
} EqualizerValues;

void equalizer_adjust(enum EqualizerControl control, int steps);

EqualizerValues equalizer_get(void);

static inline int equalizer_values_neutral(EqualizerValues v) {
    return !v.brightness && !v.gamma && !v.contrast;
}

unsigned equalizer_generation(void);

float equalizer_pl_brightness(int brightness);
float equalizer_pl_contrast(int contrast);
float equalizer_pl_gamma(int gamma);

AVFrame *equalizer_apply(AVFrame *frame);

void equalizer_uninit(void);

#endif /* LACHESIS_EQUALIZER_H */
