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
#include <limits.h>
#include <math.h>
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
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/utils/libav.h>

#include "lachesis_aspect.h"
#include "lachesis_deinterlace.h"
#include "lachesis_equalizer.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_renderer.h"
#include "lachesis_screenshot.h"
#include "lachesis_view360.h"

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

#define SCREENSHOT_MAX_SIDE 16384

typedef struct ScreenshotShape {
    int from_window;
    int rendered;
    int deinterlaced;
    int not_deinterlaced;
    int opaque;
    int for_360;
    enum View360Layout layout;
    enum View360Projection projection;
    float yaw, pitch, roll, hfov;
    int frame_w, frame_h;
    AVRational sar;
    int sar_assumed;
    int rotate;
    int scaled_w, scaled_h;
    int out_w, out_h;
    char aspect[32];
} ScreenshotShape;

static void screenshot_square_size(int w, int h, AVRational sar, int *out_w,
                                   int *out_h) {
    int64_t sw = w, sh = h;

    if (sar.num > sar.den) {
        sw = av_rescale(w, sar.num, sar.den);
        if (sw > SCREENSHOT_MAX_SIDE) {
            sw = FFMAX(w, SCREENSHOT_MAX_SIDE);
            sh = av_rescale(sw, (int64_t)h * sar.den, (int64_t)w * sar.num);
        }
    } else if (sar.num < sar.den) {
        sh = av_rescale(h, sar.den, sar.num);
        if (sh > SCREENSHOT_MAX_SIDE) {
            sh = FFMAX(h, SCREENSHOT_MAX_SIDE);
            sw = av_rescale(sh, (int64_t)w * sar.num, (int64_t)h * sar.den);
        }
    }

    *out_w = (int)FFMAX(sw, 1);
    *out_h = (int)FFMAX(sh, 1);
}

static const uint8_t *rotated_pixel(const AVFrame *img, int rotate, int x,
                                    int y) {
    int sx = x, sy = y;

    switch (rotate) {
    case 90:
        sx = y;
        sy = img->height - 1 - x;
        break;
    case 180:
        sx = img->width - 1 - x;
        sy = img->height - 1 - y;
        break;
    case 270:
        sx = img->width - 1 - y;
        sy = x;
        break;
    default:
        break;
    }

    return img->data[0] + (ptrdiff_t)sy * img->linesize[0] + 4 * (ptrdiff_t)sx;
}

static int encode_png(const char *path, AVFrame *src,
                      const ScreenshotShape *shape, const uint8_t bg[3]) {
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = NULL;
    AVFrame *rgba = NULL;
    AVFrame *rgb = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    FILE *f = NULL;
    int square_w = shape->rendered ? shape->out_w : shape->scaled_w;
    int square_h = shape->rendered ? shape->out_h : shape->scaled_h;
    int rotate = shape->rendered ? 0 : shape->rotate;
    int scaled = square_w != src->width || square_h != src->height;
    int ret;

    if (!enc) {
        return AVERROR_ENCODER_NOT_FOUND;
    }

    ctx = avcodec_alloc_context3(enc);
    if (!ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ctx->width = shape->out_w;
    ctx->height = shape->out_h;
    ctx->pix_fmt = AV_PIX_FMT_RGB24;
    ctx->sample_aspect_ratio = (AVRational){1, 1};
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
    rgba->width = square_w;
    rgba->height = square_h;
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
    rgb->width = shape->out_w;
    rgb->height = shape->out_h;
    ret = av_frame_get_buffer(rgb, 0);
    if (ret < 0) {
        goto end;
    }

    sws = sws_getContext(src->width, src->height, src->format,
                         rgba->width, rgba->height, AV_PIX_FMT_RGBA,
                         scaled ? SWS_LANCZOS | SWS_FULL_CHR_H_INT |
                                 SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND
                                : SWS_BILINEAR,
                         NULL, NULL, NULL);
    if (!sws) {
        ret = sws_isSupportedInput(src->format) ? AVERROR(ENOMEM)
                                                : AVERROR(ENOTSUP);
        goto end;
    }
    {
        int *inv_table, *table, src_range, dst_range;
        int brightness, contrast, saturation;
        if (sws_getColorspaceDetails(sws, &inv_table, &src_range, &table,
                                     &dst_range, &brightness, &contrast,
                                     &saturation) >= 0) {
            enum AVColorSpace spc = src->colorspace;
            const int *coeffs;
            if (!pl_color_system_is_ycbcr_like(pl_system_from_av(spc))) {
                spc = pl_color_system_guess_ycbcr(src->width, src->height) ==
                        PL_COLOR_SYSTEM_BT_709
                    ? AVCOL_SPC_BT709
                    : AVCOL_SPC_BT470BG;
            }
            coeffs = sws_getCoefficients(spc);
            sws_setColorspaceDetails(sws, coeffs,
                                     src->color_range == AVCOL_RANGE_JPEG,
                                     table, dst_range, brightness, contrast,
                                     saturation);
        }
    }

    sws_scale(sws, (const uint8_t *const *)src->data, src->linesize, 0,
              src->height, rgba->data, rgba->linesize);

    for (int y = 0; y < rgb->height; y++) {
        uint8_t *d = rgb->data[0] + (ptrdiff_t)y * rgb->linesize[0];
        for (int x = 0; x < rgb->width; x++) {
            const uint8_t *s = rotated_pixel(rgba, rotate, x, y);
            unsigned a = shape->opaque ? 255 : s[3];
            unsigned ia = 255 - a;
            d[3 * x + 0] = (s[0] * a + bg[0] * ia + 127) / 255;
            d[3 * x + 1] = (s[1] * a + bg[1] * ia + 127) / 255;
            d[3 * x + 2] = (s[2] * a + bg[2] * ia + 127) / 255;
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
    ScreenshotShape shape;
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

static void screenshot_finish(const char *path, const ScreenshotShape *shape,
                              int ret);

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
    screenshot_finish(job->path, &job->shape, job->ret);
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

        job->ret = encode_png(job->path, job->frame, &job->shape, job->bg);
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
                             const ScreenshotShape *shape,
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
    job->shape = *shape;
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

/* Flatten to opaque black or -video-bg. */
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
    RenderParams params;
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
    deinterlace_new_picture(is, vp);
    deinterlace_prepare(is, vp);
    params = is->render_params;
    params.osd_pixels = NULL;
    params.mix_frames = NULL;
    params.mix_num_frames = 0;
    params.mix_vsync_duration = 0.0f;
    ret = renderer_capture(renderer, vp->frame, &params, w, h, rgba->data[0],
                           rgba->linesize[0]);
    if (ret < 0) {
        av_frame_free(&rgba);
        return NULL;
    }

    return rgba;
}

static void screenshot_shape_frame(const Frame *vp, ScreenshotShape *shape) {
    int rotate = video_rotate;
    int sideways = rotate == 90 || rotate == 270;
    AVRational sar = vp->sar;

    if (sideways) {
        if (sar.num > 0 && sar.den > 0) {
            sar = av_inv_q(sar);
        }
        sar = aspect_override_sar(vp->height, vp->width, sar);
        if (sar.num > 0 && sar.den > 0) {
            sar = av_inv_q(sar);
        }
    } else {
        sar = aspect_override_sar(vp->width, vp->height, sar);
    }
    shape->sar_assumed = sar.num <= 0 || sar.den <= 0;
    if (shape->sar_assumed) {
        sar = (AVRational){1, 1};
    }
    av_reduce(&sar.num, &sar.den, sar.num, sar.den, INT_MAX);

    shape->from_window = 0;
    shape->rendered = 0;
    shape->deinterlaced = 0;
    shape->not_deinterlaced = 0;
    shape->opaque = 0;
    shape->for_360 = view360_enabled();
    shape->layout = view360_layout;
    shape->projection = view360_projection;
    shape->yaw = sbs360_yaw;
    shape->pitch = sbs360_pitch;
    shape->roll = sbs360_roll;
    shape->hfov = sbs360_hfov;
    shape->frame_w = vp->width;
    shape->frame_h = vp->height;
    shape->sar = sar;
    shape->rotate = rotate;
    screenshot_square_size(vp->width, vp->height, sar, &shape->scaled_w,
                           &shape->scaled_h);
    shape->out_w = sideways ? shape->scaled_h : shape->scaled_w;
    shape->out_h = sideways ? shape->scaled_w : shape->scaled_h;
    snprintf(shape->aspect, sizeof(shape->aspect), "%s",
             aspect_override_active() ? aspect_override_label() : "");
}

static void screenshot_shape_window(const AVFrame *shot,
                                    ScreenshotShape *shape) {
    shape->from_window = 1;
    shape->rendered = 1;
    shape->deinterlaced = 0;
    shape->not_deinterlaced = 0;
    shape->opaque = 0;
    shape->for_360 = 0;
    shape->layout = VIEW360_LAYOUT_OFF;
    shape->projection = VIEW360_PROJECTION_PANINI;
    shape->yaw = shape->pitch = shape->roll = shape->hfov = 0.0f;
    shape->frame_w = shape->scaled_w = shape->out_w = shot->width;
    shape->frame_h = shape->scaled_h = shape->out_h = shot->height;
    shape->sar = (AVRational){1, 1};
    shape->sar_assumed = 0;
    shape->rotate = 0;
    shape->aspect[0] = '\0';
}

static AVFrame *screenshot_render_frame(VideoState *is, Frame *vp,
                                        ScreenshotShape *shape,
                                        const uint8_t bg[3]) {
    int max_dim = renderer_max_texture_size(renderer);
    int w = shape->out_w, h = shape->out_h;
    RenderParams params;
    EqualizerValues eq;
    AVFrame *rgba;
    int ret;

    if (!renderer) {
        return NULL;
    }
    if (max_dim > 0 && (w > max_dim || h > max_dim)) {
        fit_within_max_dim(shape->out_w, shape->out_h, max_dim, &w, &h);
    }

    rgba = av_frame_alloc();
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

    deinterlace_new_picture(is, vp);
    deinterlace_prepare(is, vp);
    params = is->render_params;
    params.target_rect = params.target_clip = params.target_plain =
        (SDL_Rect){0, 0, w, h};
    params.osd_pixels = NULL;
    params.sub_pixels = NULL;
    params.text_sub_pixels = NULL;
    params.mix_frames = NULL;
    params.mix_num_frames = 0;
    params.mix_vsync_duration = 0.0f;
    params.rotate = shape->rotate;
    params.still_image = is->is_still_image;
    eq = equalizer_get();
    params.eq_brightness = eq.brightness;
    params.eq_gamma = eq.gamma;
    params.eq_contrast = eq.contrast;
    params.eq_saturation = eq.saturation;
    if (!shape->opaque) {
        params.video_background_type = VIDEO_BACKGROUND_COLOR;
        memcpy(params.video_background_color, bg, 3);
        params.video_background_color[3] = 255;
    }
    params.video_background_explicit = 0;

    ret = renderer_capture(renderer, vp->frame, &params, w, h, rgba->data[0],
                           rgba->linesize[0]);
    if (ret < 0) {
        log_warn("Couldn't render the screenshot, so it's converted in software: %s.\n",
                 av_err2str(ret));
        av_frame_free(&rgba);
        return NULL;
    }
    shape->out_w = w;
    shape->out_h = h;
    shape->rendered = 1;
    shape->deinterlaced = params.deinterlace != 0;

    return rgba;
}

void take_screenshot(VideoState *is, int capture_window) {
    Frame *vp;
    char path[SCREENSHOT_PATH_MAX];
    AVFrame *shot;
    ScreenshotShape shape;
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
    if (view360_enabled()) {
        renderer_update_360(renderer, sbs360_yaw, sbs360_pitch, sbs360_roll,
                            sbs360_hfov);
    }
    if (capture_window) {
        shot = screenshot_window_frame(is);
        if (shot) {
            screenshot_shape_window(shot, &shape);
        }
    } else {
        screenshot_shape_frame(vp, &shape);
        shape.opaque = is->render_params.video_background_type ==
            VIDEO_BACKGROUND_NONE;
        shot = screenshot_render_frame(is, vp, &shape, bg);
        if (!shot) {
            shot = frame_to_cpu(vp->frame);
            shape.not_deinterlaced = deinterlace;
        }
    }
    if (!shot) {
        log_warn(capture_window ? "Couldn't capture the window.\n"
                                : "Couldn't read back the video frame.\n");
        return;
    }
    if (next_screenshot_path(path, sizeof(path)) < 0) {
        log_warn("Couldn't find a free screenshot filename.\n");
        av_frame_free(&shot);
        return;
    }

    if (screenshot_submit(path, shot, &shape, bg)) {
        return;
    }

    ret = encode_png(path, shot, &shape, bg);
    av_frame_free(&shot);
    screenshot_finish(path, &shape, ret);
}

static int screenshot_yaw(const ScreenshotShape *shape) {
    float yaw = shape->yaw - view360_default_yaw(shape->layout);
    int d = (int)(lrintf(yaw) % 360);

    return d > 180 ? d - 360 : (d <= -180 ? d + 360 : d);
}

static void screenshot_describe(const ScreenshotShape *shape, char *buf,
                                size_t size) {
    size_t n;

    if (shape->from_window) {
        snprintf(buf, size, "%dx%d from the window", shape->out_w,
                 shape->out_h);
        return;
    }

    n = (size_t)snprintf(buf, size, "%dx%d", shape->out_w, shape->out_h);
    if (n < size && shape->for_360 && shape->rendered) {
        n += (size_t)snprintf(buf + n, size - n,
                              " of the %s 360 %sview at yaw %d\xc2\xb0, "
                              "pitch %d\xc2\xb0, roll %d\xc2\xb0 and HFOV "
                              "%d\xc2\xb0",
                              view360_layout_name(shape->layout),
                              shape->projection == VIEW360_PROJECTION_SPHERE
                                  ? "sphere "
                                  : "",
                              screenshot_yaw(shape), (int)lrintf(shape->pitch),
                              (int)lrintf(shape->roll),
                              (int)lrintf(shape->hfov));
    }
    if (n < size) {
        n += (size_t)snprintf(buf + n, size - n,
                              " from the %dx%d frame at %sSAR %d:%d",
                              shape->frame_w, shape->frame_h,
                              shape->sar_assumed ? "an assumed " : "",
                              shape->sar.num, shape->sar.den);
    }
    if (n < size && shape->aspect[0]) {
        n += (size_t)snprintf(buf + n, size - n, " for the %s aspect",
                              shape->aspect);
    }
    if (n < size && shape->rotate) {
        n += (size_t)snprintf(buf + n, size - n, ", rotated %d\xc2\xb0",
                              shape->rotate);
    }
    if (n < size && shape->deinterlaced) {
        n += (size_t)snprintf(buf + n, size - n, ", deinterlaced");
    }
    if (n < size && shape->not_deinterlaced) {
        n += (size_t)snprintf(buf + n, size - n, ", not deinterlaced");
    }
    if (n < size && !shape->rendered) {
        snprintf(buf + n, size - n, ", converted in software%s",
                 shape->for_360 ? " without the 360 view" : "");
    }
}

static void screenshot_finish(const char *path, const ScreenshotShape *shape,
                              int ret) {
    if (ret < 0) {
        log_warn("Failed to write screenshot %s: %s.\n", path,
                 av_err2str(ret));
        osd_show_message("Failed to save screenshot");
    } else {
        char how[256];
        char abspath[SCREENSHOT_PATH_MAX - sizeof(how) - 3];
        const char *shown = path;
        screenshot_describe(shape, how, sizeof(how));
        if (screenshot_abspath(path, abspath, sizeof(abspath)) == 0) {
            shown = abspath;
        }
        log_info("Saved screenshot %s (%s)\n", shown, how);
        const char *base = shown;
        for (const char *p = shown; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        osd_show_message("Screenshot: %s%s", *base ? base : shown,
                         shape->for_360 && !shape->rendered
                             ? " without the 360 view"
                             : "");
    }
}
