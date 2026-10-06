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

#ifndef LACHESIS_WINDOW_H
#define LACHESIS_WINDOW_H

#include "lachesis_internal.h"

extern int window_placed;

void init_default_window_size(void);
void size_default_for_content(const Frame *vp);
void video_follow_content_size(VideoState *is);

int video_open(VideoState *is);
void window_open_bare(VideoState *is);
int window_occluded(void);

void note_media_window_title(const char *path, const char *archive_path,
                             const char *entry_name);
char *startup_window_title(const char *fallback_path);
void refresh_window_title(VideoState *is);

void finish_raise(void);
void refresh_display_info(VideoState *is);
void window_uninit(void);

#endif /* LACHESIS_WINDOW_H */
