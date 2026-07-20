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

#ifndef LACHESIS_OSD_H
#define LACHESIS_OSD_H

#include <stddef.h>
#include <stdint.h>

#include <libavutil/attributes.h>

#include "lachesis_internal.h"

void osd_init_fonts(void);
void osd_uninit(void);

void osd_draw(VideoState *is);
void osd_prepare_vulkan(VideoState *is);

void osd_show_status(void);
void osd_show_seek(void);
void osd_show_volume(void);
void osd_show_position(void);
av_printf_format(1, 2) void osd_show_message(const char *fmt, ...);

typedef void (*OsdInfoProvider)(const VideoState *is, char *buf, size_t bufsz);
void osd_set_info_provider(OsdInfoProvider provider);
void osd_set_stats_provider(OsdInfoProvider provider);
void osd_toggle_info_page(int page);
void osd_invalidate_info(void);

void osd_show_delete_prompt(const char *name);
void osd_hide_delete_prompt(void);

int64_t osd_visible_until(void);
void osd_reset_timers(void);

void format_time(char *buf, int bufsz, double secs);

#endif /* LACHESIS_OSD_H */
