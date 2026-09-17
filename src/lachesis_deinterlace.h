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

#ifndef LACHESIS_DEINTERLACE_H
#define LACHESIS_DEINTERLACE_H

#include <libavutil/frame.h>

#include "lachesis_renderer.h"

typedef struct Frame Frame;
typedef struct VideoState VideoState;

struct pl_frame;
struct pl_render_params;

int deinterlace_active(const VideoState *is);

const char *deinterlace_status(const VideoState *is);

void deinterlace_new_picture(VideoState *is, const Frame *vp);
void deinterlace_prepare(VideoState *is, Frame *vp);
void deinterlace_clear(VideoState *is);
void deinterlace_retire_frame(VideoState *is);
void deinterlace_pace(VideoState *is, double delay, double time,
                      double *remaining_time);
void deinterlace_close(VideoState *is);

void deinterlace_apply(struct pl_frame *pl_frame,
                       struct pl_render_params *pl_params,
                       const AVFrame *frame, const RenderParams *params);

#endif /* LACHESIS_DEINTERLACE_H */
