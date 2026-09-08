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

#ifndef LACHESIS_HWACCEL_H
#define LACHESIS_HWACCEL_H

#include <stddef.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>

typedef struct HwDownload {
    AVBufferPool *pool;
    enum AVPixelFormat format;
    enum AVPixelFormat sw_format;
    int width;
    int height;
} HwDownload;

int hwdownload_frame(HwDownload *dl, AVFrame *dst, const AVFrame *src);
void hwdownload_free(HwDownload *dl);

int hwaccel_glob_match(const char *pattern, const char *text);

#define HWACCEL_EXTRA_FRAMES 6

int hwaccel_open_device(AVBufferRef **device_ctx, const AVCodec *codec,
                        const AVCodecContext *avctx, AVRational frame_rate);

#endif /* LACHESIS_HWACCEL_H */
