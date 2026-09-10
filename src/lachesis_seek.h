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

#ifndef LACHESIS_SEEK_H
#define LACHESIS_SEEK_H

#include <stdint.h>

#include "lachesis_internal.h"

#define PLAYBACK_SPEED_STEP 0.1

extern double playback_speed;
extern double ab_loop_a;
extern double ab_loop_b;

double get_clock(Clock *c);
void set_clock(Clock *c, double pts, int serial);
void set_clock_at(Clock *c, double pts, int serial, double time);
void init_clock(Clock *c, int *queue_serial);
void sync_clock_to_slave(Clock *c, Clock *slave);
void external_clock_reseat(VideoState *is, Clock *slave);
void check_external_clock_speed(VideoState *is);
int get_master_sync_type(VideoState *is);
double get_master_clock(VideoState *is);
void reanchor_clocks(VideoState *is);

int video_stream_advances(VideoState *is);
double playhead_origin(const VideoState *is);
double playhead_length(const VideoState *is);
double playhead_elapsed(const VideoState *is, double pos);
double playhead_clamp(const VideoState *is, double pos);
double effective_playhead(VideoState *is);
double aligned_start_pts(VideoState *is);

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes);
void stream_seek_exact(VideoState *is, int64_t pos);

/* An exact seek lands on a keyframe ahead of the target and then throws away
 * what it decodes until the target arrives. */
void exact_seek_arm(VideoState *is, int64_t target);
void exact_seek_cancel(VideoState *is);
int exact_seek_drop_video(VideoState *is, double pts);
int exact_seek_drop_audio(VideoState *is, double pts, double duration);

void stream_toggle_pause(VideoState *is);
void toggle_pause(VideoState *is);
void step_to_next_frame(VideoState *is);
void frame_step(VideoState *is);

void set_playback_speed(VideoState *is, double speed);
void reset_playback_speed(void);

int ab_loop_defining(void);
void ab_loop_toggle(VideoState *is);
void ab_loop_check(VideoState *is);
void ab_loop_reset(void);

#endif /* LACHESIS_SEEK_H */
