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

#ifndef LACHESIS_NETWORK_H
#define LACHESIS_NETWORK_H

#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include "lachesis_internal.h"

void set_ytdl_http_opts(AVDictionary **opts);

int ytdl_resolve(const char *url, char **video_url, char **audio_url);

/* XXX */
struct YtdlChunkedIO;

struct YtdlChunkedIO *ytdl_chunked_create(const char *url, VideoState *is);
void ytdl_chunked_free(struct YtdlChunkedIO **pc);
AVIOContext *ytdl_chunked_pb(struct YtdlChunkedIO *c);

#endif /* LACHESIS_NETWORK_H */
