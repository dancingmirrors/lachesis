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

#ifndef LACHESIS_APPEND_H
#define LACHESIS_APPEND_H

#include <libavformat/avformat.h>
#include <libavformat/avio.h>

#include "lachesis_internal.h"

struct AppendIO;

int append_io_local_file(const char *url);
int append_io_applies(const char *url, const AVInputFormat *forced);

struct AppendIO *append_io_create(const char *url, VideoState *is);
struct AppendIO *append_io_create_reader(const char *url, VideoState *is);
void append_io_free(struct AppendIO **pa);
AVIOContext *append_io_pb(struct AppendIO *a);

unsigned append_io_ops(const struct AppendIO *a);
int append_io_wait_growth(struct AppendIO *a);

int64_t append_io_read(struct AppendIO *a, void *buf, size_t size,
                       int expect_more);
int64_t append_io_seek(struct AppendIO *a, int64_t offset, int whence);

#endif /* LACHESIS_APPEND_H */
