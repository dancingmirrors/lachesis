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

#ifndef LACHESIS_NORMALIZE_H
#define LACHESIS_NORMALIZE_H

#include <stdint.h>

#include <libavutil/channel_layout.h>

#include "lachesis_internal.h"

#define NORMALIZE_TARGET_MIN (-40.0)
#define NORMALIZE_TARGET_MAX (-5.0)
#define NORMALIZE_GAIN_MIN (-6.0)
#define NORMALIZE_GAIN_MAX 12.0

#define NORMALIZE_BOOST_MAX 24.0
#define NORMALIZE_CUT_MAX (-24.0)

void normalize_init(void);

void normalize_toggle(VideoState *is);
int normalize_enabled(void);

void normalize_reset(void);

void normalize_process(int16_t *samples, int nb_frames, int nb_channels,
                       int sample_rate, const AVChannelLayout *ch_layout);

const char *normalize_status(void);

#endif /* LACHESIS_NORMALIZE_H */
