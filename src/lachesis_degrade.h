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

#ifndef LACHESIS_DEGRADE_H
#define LACHESIS_DEGRADE_H

#include <stdint.h>

#include <libavcodec/avcodec.h>

typedef struct VideoState VideoState;

void apply_degraded_decode(AVCodecContext *avctx, int level);
void degrade_init(VideoState *is);
void degrade_reset(VideoState *is);
void degrade_note_stall(VideoState *is, int64_t stall_us);
void degrade_frame(VideoState *is, double dpts, int64_t decode_us,
                   int64_t budget_us, int had_packets);

int degrade_can_catch_up(const VideoState *is, double now);
double degrade_read_ahead_secs(const VideoState *is, double base);
int degrade_stale_frame(VideoState *is, double pts, int serial);
int degrade_drop_late_frame(VideoState *is, double dpts, int64_t interval_us);

const char *degrade_status(const VideoState *is);

#endif /* LACHESIS_DEGRADE_H */
