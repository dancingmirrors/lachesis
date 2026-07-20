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

#ifndef LACHESIS_INFORMATION_H
#define LACHESIS_INFORMATION_H

#include <stddef.h>

#include <libavutil/rational.h>

#include "lachesis_internal.h"

void print_current_file(const VideoState *is);
void print_stream_info(const VideoState *is);

void format_media_info(const VideoState *is, char *buf, size_t bufsz);
void format_playback_stats(const VideoState *is, char *buf, size_t bufsz);

void media_info_reset(void);
void media_info_set_hwaccel(const char *name);
void media_info_note_audio_driver(const char *driver, int channels, int freq);
void media_info_note_audio_format(unsigned fmt, int channels, int freq,
                                  int buffer_frames);
void media_info_note_video_output(int width, int height, AVRational sar);

#endif /* LACHESIS_INFORMATION_H */
