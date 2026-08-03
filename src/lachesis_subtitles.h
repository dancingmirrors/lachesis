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

#ifndef LACHESIS_SUBTITLE_H
#define LACHESIS_SUBTITLE_H

#include <SDL3/SDL.h>

#include <libavcodec/avcodec.h>

#include "lachesis_internal.h"

int subtitle_thread(void *arg);
int open_external_subtitle(VideoState *is);
void close_external_subtitle(VideoState *is);

typedef struct SubtitleOverlay {
    SDL_Surface *surf;
    int x, y;
    unsigned generation;
} SubtitleOverlay;

void subtitles_init(void);

int subtitles_track_open(AVCodecContext *avctx);
void subtitles_track_close(void);

void subtitles_track_flush(void);
void subtitles_uninit(void);
void subtitles_reap(void);

int subtitles_track_attached(void);
int subtitles_visible_at(double now);
int subtitles_render(VideoState *is, int canvas_w, int canvas_h,
                     const SDL_Rect *video_rect, double now,
                     SubtitleOverlay *out);

#endif /* LACHESIS_SUBTITLE_H */
