/*
 * Copyright © 2003 Fabrice Bellard
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

#ifndef LACHESIS_AUDIO_H
#define LACHESIS_AUDIO_H

#include <stdint.h>

#include <libavutil/channel_layout.h>

#include "lachesis_internal.h"

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#define SAMPLE_CORRECTION_PERCENT_MAX 10

extern int64_t audio_callback_time;

/* Bumped whenever the playback speed changes so the audio thread rebuilds its
 * atempo filter chain. */
extern int audio_speed_serial;

int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout,
               int wanted_sample_rate, struct AudioParams *audio_hw_params);
int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);
int audio_thread(void *arg);

void audio_device_resume(void);
void audio_device_close(void);

#endif /* LACHESIS_AUDIO_H */
