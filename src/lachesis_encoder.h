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

#ifndef LACHESIS_ENCODER_H
#define LACHESIS_ENCODER_H

#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>

#include "lachesis_internal.h"

const AVOutputFormat *encoder_output_format(const char *path);

int encoder_enabled(void);
int encoder_renders(const VideoState *is);
int encoder_init(void);

double encoder_refresh_rate(void);

void encoder_note_input(const AVFormatContext *ic);

const enum AVPixelFormat *encoder_pix_fmts(int *count);

int encoder_open_audio(const AVChannelLayout *layout, int sample_rate,
                       struct AudioParams *tgt);

av_noreturn void encoder_run(VideoState *is);

#endif // LACHESIS_ENCODER_H
