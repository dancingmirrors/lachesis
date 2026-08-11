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

#ifndef LACHESIS_INTERPOLATE_H
#define LACHESIS_INTERPOLATE_H

#include "lachesis_internal.h"
#include "lachesis_renderer.h"

int interpolate_frames(VideoState *is, Frame *vp, RenderMixFrame *mix,
                       float *vsync_duration);
int interpolate_pace(VideoState *is, double now, double *remaining_time);

const char *interpolate_status(void);

#endif /* LACHESIS_INTERPOLATE_H */
