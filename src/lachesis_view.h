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

#ifndef LACHESIS_VIEW_H
#define LACHESIS_VIEW_H

#include <SDL3/SDL.h>

#include <libavutil/rational.h>

#include "lachesis_internal.h"

void calculate_display_rect(SDL_Rect *rect, SDL_Rect *clip, SDL_Rect *plain,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar);

float view_zoom_step(VideoState *is, int direction);
float view_zoom_reset(VideoState *is);
void view_pan_by(VideoState *is, float dx, float dy);

#endif /* LACHESIS_VIEW_H */
