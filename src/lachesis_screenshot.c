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

#include "lachesis_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define PATH_SEPARATOR '\\'
#else
#include <unistd.h>
#define PATH_SEPARATOR '/'
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_renderer.h"
#include "lachesis_screenshot.h"

static int screenshot_abspath(const char *path, char *out, size_t out_size) {
    char cwd[4078];
#if defined(_WIN32)
    if (!_getcwd(cwd, (int)sizeof(cwd))) {
        return -1;
    }
#else
    if (!getcwd(cwd, sizeof(cwd))) {
        return -1;
    }
#endif
    if (snprintf(out, out_size, "%s%c%s", cwd, PATH_SEPARATOR, path) >= (int)out_size) {
        return -1;
    }
    return 0;
}

static int next_screenshot_path(char *out, size_t out_size) {
    for (int i = 1; i <= 9999; i++) {
        struct stat st;
        snprintf(out, out_size, "lachesis-%04d.png", i);
        if (stat(out, &st) != 0) {
            return 0;
        }
    }

    return -1;
}

static int encode_png(const char *path, AVFrame *src, const uint8_t bg[3]) {
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = NULL;
    AVFrame *rgba = NULL;
    AVFrame *rgb = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    FILE *f = NULL;
    int ret;

    if (!enc) {
        return AVERROR_ENCODER_NOT_FOUND;
    }

    ctx = avcodec_alloc_context3(enc);
    if (!ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ctx->width = src->width;
    ctx->height = src->height;
    ctx->pix_fmt = AV_PIX_FMT_RGB24;
    ctx->time_base = (AVRational){1, 25};
    ret = avcodec_open2(ctx, enc, NULL);
    if (ret < 0) {
        goto end;
    }

    rgba = av_frame_alloc();
    if (!rgba) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = src->width;
    rgba->height = src->height;
    ret = av_frame_get_buffer(rgba, 0);
    if (ret < 0) {
        goto end;
    }

    rgb = av_frame_alloc();
    if (!rgb) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = src->width;
    rgb->height = src->height;
    ret = av_frame_get_buffer(rgb, 0);
    if (ret < 0) {
        goto end;
    }

    sws = sws_getContext(src->width, src->height, src->format,
                         src->width, src->height, AV_PIX_FMT_RGBA,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    {
        int *inv_table, *table, src_range, dst_range;
        int brightness, contrast, saturation;
        if (sws_getColorspaceDetails(sws, &inv_table, &src_range, &table,
                                     &dst_range, &brightness, &contrast,
                                     &saturation) >= 0) {
            const int *coeffs = sws_getCoefficients(src->colorspace);
            sws_setColorspaceDetails(sws, coeffs,
                                     src->color_range == AVCOL_RANGE_JPEG,
                                     table, dst_range, brightness, contrast,
                                     saturation);
        }
    }

    sws_scale(sws, (const uint8_t *const *)src->data, src->linesize, 0,
              src->height, rgba->data, rgba->linesize);

    for (int y = 0; y < rgb->height; y++) {
        const uint8_t *s = rgba->data[0] + (ptrdiff_t)y * rgba->linesize[0];
        uint8_t *d = rgb->data[0] + (ptrdiff_t)y * rgb->linesize[0];
        for (int x = 0; x < rgb->width; x++) {
            unsigned a = s[4 * x + 3];
            unsigned ia = 255 - a;
            d[3 * x + 0] = (s[4 * x + 0] * a + bg[0] * ia + 127) / 255;
            d[3 * x + 1] = (s[4 * x + 1] * a + bg[1] * ia + 127) / 255;
            d[3 * x + 2] = (s[4 * x + 2] * a + bg[2] * ia + 127) / 255;
        }
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_send_frame(ctx, rgb);
    if (ret < 0) {
        goto end;
    }
    avcodec_send_frame(ctx, NULL);
    ret = avcodec_receive_packet(ctx, pkt);
    if (ret < 0) {
        goto end;
    }

    f = fopen(path, "wb");
    if (!f) {
        ret = AVERROR(errno);
        goto end;
    }
    if (fwrite(pkt->data, 1, pkt->size, f) != (size_t)pkt->size) {
        ret = AVERROR(EIO);
        goto end;
    }
    ret = 0;

end:
    if (f) {
        fclose(f);
    }
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&rgb);
    av_frame_free(&rgba);
    avcodec_free_context(&ctx);

    return ret;
}

/* Flatten to opaque black or -video_bg. */
static void screenshot_bg_color(VideoState *is, uint8_t bg[3]) {
    if (is->render_params.video_background_type == VIDEO_BACKGROUND_COLOR) {
        bg[0] = is->render_params.video_background_color[0];
        bg[1] = is->render_params.video_background_color[1];
        bg[2] = is->render_params.video_background_color[2];
    } else {
        bg[0] = bg[1] = bg[2] = 0;
    }
}

static AVFrame *frame_to_cpu(AVFrame *frame) {
    AVFrame *sw;

    if (!frame->hw_frames_ctx) {
        return av_frame_clone(frame);
    }
    sw = av_frame_alloc();
    if (!sw) {
        return NULL;
    }
    if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
        av_frame_free(&sw);
        return NULL;
    }
    av_frame_copy_props(sw, frame);

    return sw;
}

static int screenshot_window(VideoState *is, const char *path) {
    Frame *vp = frame_queue_peek_last(&is->pictq);
    int w = 0, h = 0;
    int ret;
    uint8_t bg[3];

    screenshot_bg_color(is, bg);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return AVERROR(EINVAL);
    }

    if (vk_renderer) {
        AVFrame *rgba = av_frame_alloc();
        if (!rgba) {
            return AVERROR(ENOMEM);
        }
        rgba->format = AV_PIX_FMT_RGBA;
        rgba->width = w;
        rgba->height = h;
        ret = av_frame_get_buffer(rgba, 0);
        if (ret < 0) {
            av_frame_free(&rgba);
            return ret;
        }
        calculate_display_rect(&is->render_params.target_rect, is->xleft,
                               is->ytop, is->width, is->height, vp->width,
                               vp->height, vp->sar);
        is->render_params.rotate = video_rotate;
        ret = vk_renderer_capture(vk_renderer, vp->frame, &is->render_params,
                                  w, h, rgba->data[0], rgba->linesize[0]);
        if (ret >= 0) {
            ret = encode_png(path, rgba, bg);
        }
        av_frame_free(&rgba);
        return ret;
    }

    SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *conv;
    AVFrame *frame;

    if (!surf) {
        return AVERROR_EXTERNAL;
    }
    conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) {
        return AVERROR_EXTERNAL;
    }
    frame = av_frame_alloc();
    if (!frame) {
        SDL_DestroySurface(conv);
        return AVERROR(ENOMEM);
    }
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = conv->w;
    frame->height = conv->h;
    frame->data[0] = conv->pixels;
    frame->linesize[0] = conv->pitch;
    ret = encode_png(path, frame, bg);
    av_frame_free(&frame);
    SDL_DestroySurface(conv);

    return ret;
}

void take_screenshot(VideoState *is, int capture_window) {
    Frame *vp;
    char path[4078];
    int ret;

    if (!is) {
        return;
    }
    vp = frame_queue_peek_last(&is->pictq);
    if (!vp || !vp->frame || !vp->width || !vp->height) {
        log_warn("No video frame to capture.\n");
        return;
    }
    if (next_screenshot_path(path, sizeof(path)) < 0) {
        log_warn("Couldn't find a free screenshot filename.\n");
        return;
    }

    if (capture_window) {
        ret = screenshot_window(is, path);
    } else {
        uint8_t bg[3];
        AVFrame *cpu = frame_to_cpu(vp->frame);
        if (!cpu) {
            log_warn("Couldn't read back the video frame.\n");
            return;
        }
        screenshot_bg_color(is, bg);
        ret = encode_png(path, cpu, bg);
        av_frame_free(&cpu);
    }

    if (ret < 0) {
        log_warn("Failed to write screenshot %s.\n", path);
        osd_show_message("Failed to save screenshot");
    } else {
        char abspath[4078];
        const char *shown = path;
        if (screenshot_abspath(path, abspath, sizeof(abspath)) == 0) {
            log_info("Saved screenshot %s\n", abspath);
            shown = abspath;
        } else {
            log_info("Saved screenshot %s\n", path);
        }
        const char *base = shown;
        for (const char *p = shown; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        osd_show_message("Screenshot: %s", *base ? base : shown);
    }
}
