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
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include "lachesis_deinterlace.h"
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
    static int next_index = 1;

    for (; next_index <= 9999; next_index++) {
        struct stat st;
        snprintf(out, out_size, "lachesis-%04d.png", next_index);
        if (stat(out, &st) != 0) {
            next_index++;
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

#define SCREENSHOT_PATH_MAX 4078
#define SCREENSHOT_DRAIN_TIMEOUT_US (5 * 1000000)

typedef struct ScreenshotJob {
    struct ScreenshotJob *next;
    AVFrame *frame;
    uint8_t bg[3];
    int ret;
    char path[SCREENSHOT_PATH_MAX];
} ScreenshotJob;

static SDL_Thread *screenshot_tid;
static SDL_Mutex *screenshot_lock;
static SDL_Condition *screenshot_cond;
static ScreenshotJob *screenshot_head;
static ScreenshotJob *screenshot_tail;
static int screenshot_quit;
static int screenshot_shut_down;
static SDL_AtomicInt screenshot_thread_done;

static void screenshot_job_free(ScreenshotJob *job) {
    if (!job) {
        return;
    }
    av_frame_free(&job->frame);
    av_free(job);
}

static void screenshot_finish(const char *path, int ret);

static void screenshot_post(ScreenshotJob *job) {
    SDL_Event event;

    SDL_zero(event);
    event.type = FF_SCREENSHOT_EVENT;
    event.user.data1 = job;
    if (!SDL_PushEvent(&event)) {
        SDL_ClearError();
        screenshot_job_free(job);
    }
}

void screenshot_report(const SDL_Event *event) {
    ScreenshotJob *job = event->user.data1;

    if (!job) {
        return;
    }
    screenshot_finish(job->path, job->ret);
    screenshot_job_free(job);
}

static int screenshot_thread(void *unused) {
    (void)unused;

    thread_set_priority(SDL_THREAD_PRIORITY_LOW, "screenshot encoder");

    for (;;) {
        ScreenshotJob *job;

        SDL_LockMutex(screenshot_lock);
        while (!screenshot_quit && !screenshot_head) {
            SDL_WaitCondition(screenshot_cond, screenshot_lock);
        }
        job = screenshot_head;
        if (!job) {
            SDL_UnlockMutex(screenshot_lock);
            break;
        }
        screenshot_head = job->next;
        if (!screenshot_head) {
            screenshot_tail = NULL;
        }
        SDL_UnlockMutex(screenshot_lock);

        job->ret = encode_png(job->path, job->frame, job->bg);
        av_frame_free(&job->frame);
        screenshot_post(job);
    }

    SDL_SetAtomicInt(&screenshot_thread_done, 1);

    return 0;
}

static int screenshot_thread_start(void) {
    if (screenshot_tid) {
        return 1;
    }
    if (screenshot_shut_down) {
        return 0;
    }
    if (!screenshot_lock && !(screenshot_lock = SDL_CreateMutex())) {
        return 0;
    }
    if (!screenshot_cond && !(screenshot_cond = SDL_CreateCondition())) {
        return 0;
    }
    screenshot_tid = SDL_CreateThread(screenshot_thread, "screenshot", NULL);

    return screenshot_tid != NULL;
}

static int screenshot_submit(const char *path, AVFrame *frame,
                             const uint8_t bg[3]) {
    ScreenshotJob *job;

    if (!screenshot_thread_start()) {
        return 0;
    }
    job = av_mallocz(sizeof(*job));
    if (!job) {
        return 0;
    }
    snprintf(job->path, sizeof(job->path), "%s", path);
    job->frame = frame;
    memcpy(job->bg, bg, sizeof(job->bg));

    SDL_LockMutex(screenshot_lock);
    if (screenshot_tail) {
        screenshot_tail->next = job;
    } else {
        screenshot_head = job;
    }
    screenshot_tail = job;
    SDL_SignalCondition(screenshot_cond);
    SDL_UnlockMutex(screenshot_lock);

    return 1;
}

int screenshot_shutdown(void) {
    int64_t deadline;
    SDL_Event event;
    int joined = 1;

    if (!screenshot_tid) {
        screenshot_shut_down = 1;
        return 1;
    }

    SDL_LockMutex(screenshot_lock);
    screenshot_quit = 1;
    screenshot_shut_down = 1;
    SDL_SignalCondition(screenshot_cond);
    SDL_UnlockMutex(screenshot_lock);

    deadline = av_gettime_relative() + SCREENSHOT_DRAIN_TIMEOUT_US;
    while (!SDL_GetAtomicInt(&screenshot_thread_done)) {
        if (av_gettime_relative() >= deadline) {
            log_warn("Giving up on a screenshot that is still encoding.\n");
            SDL_DetachThread(screenshot_tid);
            joined = 0;
            break;
        }
        SDL_Delay(1);
    }
    if (joined) {
        SDL_WaitThread(screenshot_tid, NULL);
    }
    screenshot_tid = NULL;

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, FF_SCREENSHOT_EVENT,
                          FF_SCREENSHOT_EVENT) > 0) {
        screenshot_report(&event);
    }

    if (joined) {
        SDL_DestroyCondition(screenshot_cond);
        screenshot_cond = NULL;
        SDL_DestroyMutex(screenshot_lock);
        screenshot_lock = NULL;
    }

    return joined;
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
    int ret;

    if (!frame->hw_frames_ctx) {
        sw = av_frame_clone(frame);
        if (!sw) {
            return NULL;
        }
    } else {
        sw = av_frame_alloc();
        if (!sw) {
            return NULL;
        }
        ret = av_hwframe_transfer_data(sw, frame, 0);
        if (ret < 0) {
            log_warn("Couldn't read back the video frame: %s.\n",
                     av_err2str(ret));
            av_frame_free(&sw);
            return NULL;
        }
        ret = av_frame_copy_props(sw, frame);
        if (ret < 0) {
            log_warn("Couldn't copy the frame properties: %s.\n",
                     av_err2str(ret));
            av_frame_free(&sw);
            return NULL;
        }
    }

    if (sw->crop_left || sw->crop_top || sw->crop_right || sw->crop_bottom) {
        ret = av_frame_apply_cropping(sw, AV_FRAME_CROP_UNALIGNED);
        if (ret < 0) {
            log_warn("Failed to apply frame cropping: %s.\n", av_err2str(ret));
            av_frame_free(&sw);
            return NULL;
        }
    }

    if (sw->width <= 0 || sw->height <= 0) {
        log_warn("The video frame is empty after cropping.\n");
        av_frame_free(&sw);
        return NULL;
    }

    return sw;
}

static AVFrame *screenshot_window_frame(VideoState *is) {
    Frame *vp = frame_queue_peek_last(&is->pictq);
    int w = 0, h = 0;
    int ret;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    AVFrame *rgba = av_frame_alloc();

    if (!rgba) {
        return NULL;
    }
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = w;
    rgba->height = h;
    ret = av_frame_get_buffer(rgba, 0);
    if (ret < 0) {
        av_frame_free(&rgba);
        return NULL;
    }
    video_prepare_overlays(is);
    is->render_params.rotate = video_rotate;
    is->render_params.still_image = is->is_still_image;
    deinterlace_prepare(is, vp);
    ret = renderer_capture(renderer, vp->frame, &is->render_params,
                           w, h, rgba->data[0], rgba->linesize[0]);
    if (ret < 0) {
        av_frame_free(&rgba);
        return NULL;
    }

    return rgba;
}

void take_screenshot(VideoState *is, int capture_window) {
    Frame *vp;
    char path[SCREENSHOT_PATH_MAX];
    AVFrame *shot;
    uint8_t bg[3];
    int ret;

    if (!is) {
        return;
    }
    vp = frame_queue_peek_last(&is->pictq);
    if (!vp || !vp->frame || !vp->width || !vp->height) {
        log_warn("No video frame to capture.\n");
        return;
    }

    screenshot_bg_color(is, bg);
    shot = capture_window ? screenshot_window_frame(is) : frame_to_cpu(vp->frame);
    if (!shot) {
        log_warn("Couldn't read back the video frame.\n");
        return;
    }
    if (next_screenshot_path(path, sizeof(path)) < 0) {
        log_warn("Couldn't find a free screenshot filename.\n");
        av_frame_free(&shot);
        return;
    }

    if (screenshot_submit(path, shot, bg)) {
        return;
    }

    ret = encode_png(path, shot, bg);
    av_frame_free(&shot);
    screenshot_finish(path, ret);
}

static void screenshot_finish(const char *path, int ret) {
    if (ret < 0) {
        log_warn("Failed to write screenshot %s.\n", path);
        osd_show_message("Failed to save screenshot");
    } else {
        char abspath[SCREENSHOT_PATH_MAX];
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
