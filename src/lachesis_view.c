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

#include <math.h>

#include <libavutil/mathematics.h>

#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_view.h"
#include "lachesis_view360.h"

#define VIEW_ZOOM_STEP 1.1f
#define VIEW_ZOOM_MIN 0.05f
#define VIEW_ZOOM_MAX 8.0f

static float view_zoom_want;
static float view_zoom_plain;
static float view_center_s = 0.5f;
static float view_center_t = 0.5f;
static float display_scale = 1.0f;
static float display_pan_x;
static float display_pan_y;

typedef struct DisplaySizes {
    int64_t fit_w, fit_h;
    int64_t nat_w, nat_h;
    int64_t base_w, base_h;
} DisplaySizes;

static void display_sizes(int scr_width, int scr_height, int pic_width,
                          int pic_height, AVRational pic_sar,
                          DisplaySizes *out) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height;

    if (pic_width < 1) {
        pic_width = 1;
    }
    if (pic_height < 1) {
        pic_height = 1;
    }
    scr_width = FFMAX(scr_width, 1);
    scr_height = FFMAX(scr_height, 1);

    if (video_rotate == 90 || video_rotate == 270) {
        int tmp = pic_width;
        pic_width = pic_height;
        pic_height = tmp;
        if (aspect_ratio.num > 0 && aspect_ratio.den > 0) {
            aspect_ratio = av_make_q(aspect_ratio.den, aspect_ratio.num);
        }
    }

    if (aspect_ratio.num <= 0 || aspect_ratio.den <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    out->fit_w = FFMAX(width, 1);
    out->fit_h = FFMAX(height, 1);

    height = pic_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    out->nat_w = FFMAX(width, 1);
    out->nat_h = FFMAX(height, 1);

    if (video_fill || out->nat_w > out->fit_w || out->nat_h > out->fit_h) {
        out->base_w = out->fit_w;
        out->base_h = out->fit_h;
    } else {
        out->base_w = out->nat_w;
        out->base_h = out->nat_h;
    }
}

static void view_center_to_display(float *u, float *v) {
    switch (video_rotate) {
    case 90:
        *u = 1.0f - view_center_t;
        *v = view_center_s;
        break;
    case 180:
        *u = 1.0f - view_center_s;
        *v = 1.0f - view_center_t;
        break;
    case 270:
        *u = view_center_t;
        *v = 1.0f - view_center_s;
        break;
    default:
        *u = view_center_s;
        *v = view_center_t;
        break;
    }
}

static void view_center_from_display(float u, float v) {
    switch (video_rotate) {
    case 90:
        view_center_s = v;
        view_center_t = 1.0f - u;
        break;
    case 180:
        view_center_s = 1.0f - u;
        view_center_t = 1.0f - v;
        break;
    case 270:
        view_center_s = 1.0f - v;
        view_center_t = u;
        break;
    default:
        view_center_s = u;
        view_center_t = v;
        break;
    }
}

static void view_pan_limits(int64_t width, int64_t height, int64_t clip_w,
                            int64_t clip_h, float *max_x, float *max_y) {
    *max_x = (float)(FFABS(width - clip_w) / 2);
    *max_y = (float)(FFABS(height - clip_h) / 2);
}

static void view_log(int scr_width, int scr_height, const DisplaySizes *sizes,
                     int64_t width, int64_t height) {
    static int64_t last[8];
    int64_t now[8] = {is_fullscreen, video_rotate, scr_width, scr_height,
                      sizes->nat_w, sizes->nat_h, width, height};

    if (!memcmp(now, last, sizeof(now))) {
        return;
    }
    memcpy(last, now, sizeof(now));

    log_verbose("View: %s %dx%d, box %dx%d, picture %dx%d at %.0f%% of the fit "
                "and %.0f%% of native, showing %.0f%% by %.0f%%.\n",
                is_fullscreen ? "screen" : "window", scr_width, scr_height,
                (int)sizes->fit_w, (int)sizes->fit_h, (int)width, (int)height,
                (double)((float)width / (float)sizes->fit_w * 100.0f),
                (double)((float)width / (float)sizes->nat_w * 100.0f),
                (double)(FFMIN((float)sizes->fit_w / (float)width, 1.0f) * 100.0f),
                (double)(FFMIN((float)sizes->fit_h / (float)height, 1.0f) * 100.0f));
}

static float view_zoom_settled(float plain) {
    if (view_zoom_want <= 0.0f) {
        return plain;
    }
    if (view_zoom_plain > 0.0f && view_zoom_want < view_zoom_plain) {
        return view_zoom_want / view_zoom_plain * plain;
    }

    return FFMAX(view_zoom_want, plain);
}

void calculate_display_rect(SDL_Rect *rect, SDL_Rect *clip, SDL_Rect *plain,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar) {
    DisplaySizes sizes;
    int64_t width, height, x, y;
    float max_pan_x, max_pan_y, u, v;

    scr_width = FFMAX(scr_width, 1);
    scr_height = FFMAX(scr_height, 1);
    display_sizes(scr_width, scr_height, pic_width, pic_height, pic_sar, &sizes);

    if (clip) {
        clip->x = scr_xleft + (int)((scr_width - sizes.fit_w) / 2);
        clip->y = scr_ytop + (int)((scr_height - sizes.fit_h) / 2);
        clip->w = (int)sizes.fit_w;
        clip->h = (int)sizes.fit_h;
    }
    if (plain) {
        plain->x = scr_xleft + (int)((scr_width - sizes.base_w) / 2);
        plain->y = scr_ytop + (int)((scr_height - sizes.base_h) / 2);
        plain->w = (int)sizes.base_w;
        plain->h = (int)sizes.base_h;
    }

    if (view_zoom_want > 0.0f) {
        float zoom = view_zoom_settled((float)sizes.base_w / (float)sizes.fit_w);

        width = FFMAX((int64_t)((float)sizes.fit_w * zoom), 1);
        height = FFMAX((int64_t)((float)sizes.fit_h * zoom), 1);
    } else {
        width = sizes.base_w;
        height = sizes.base_h;
    }
    display_scale = (float)width / (float)sizes.fit_w;
    view_log(scr_width, scr_height, &sizes, width, height);

    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;

    view_center_to_display(&u, &v);
    display_pan_x = (float)width * (0.5f - u);
    display_pan_y = (float)height * (0.5f - v);
    view_pan_limits(width, height, sizes.fit_w, sizes.fit_h, &max_pan_x, &max_pan_y);
    display_pan_x = av_clipf(display_pan_x, -max_pan_x, max_pan_x);
    display_pan_y = av_clipf(display_pan_y, -max_pan_y, max_pan_y);
    x += lrintf(display_pan_x);
    y += lrintf(display_pan_y);

    rect->x = scr_xleft + (int)x;
    rect->y = scr_ytop + (int)y;
    rect->w = FFMAX((int)width, 1);
    rect->h = FFMAX((int)height, 1);
}

static int view_picture(VideoState *is, Frame **out) {
    Frame *vp;

    if (!is->video_st || is->width <= 0 || is->height <= 0) {
        return 0;
    }
    vp = frame_queue_peek_last(&is->pictq);
    if (vp->width <= 0 || vp->height <= 0) {
        return 0;
    }
    *out = vp;

    return 1;
}

static float view_measure(VideoState *is, const Frame *vp, SDL_Rect *rect,
                          SDL_Rect *clip) {
    SDL_Rect unwanted;

    calculate_display_rect(rect ? rect : &unwanted, clip, NULL, 0, 0, is->width,
                           is->height, vp->width, vp->height, vp->sar);

    return display_scale;
}

static float view_default_zoom(const VideoState *is, const Frame *vp) {
    DisplaySizes sizes;

    display_sizes(is->width, is->height, vp->width, vp->height, vp->sar, &sizes);

    return (float)sizes.base_w / (float)sizes.fit_w;
}

float view_zoom_step(VideoState *is, int direction) {
    Frame *vp;
    float settled, want, plain;

    if (!view_picture(is, &vp)) {
        return 0.0f;
    }
    plain = view_default_zoom(is, vp);

    settled = view_zoom_settled(plain);
    want = settled * (direction > 0 ? VIEW_ZOOM_STEP : 1.0f / VIEW_ZOOM_STEP);
    want = av_clipf(want, FFMIN(VIEW_ZOOM_MIN, plain / VIEW_ZOOM_MAX),
                    VIEW_ZOOM_MAX);

    if ((settled - plain) * (want - plain) < 0.0f ||
        fabsf(want - plain) < plain * 0.005f) {
        view_zoom_want = 0.0f;
    } else {
        view_zoom_want = want;
        view_zoom_plain = plain;
    }

    return view_measure(is, vp, NULL, NULL);
}

float view_zoom_reset(VideoState *is) {
    Frame *vp;

    view_zoom_want = 0.0f;
    view_zoom_plain = 0.0f;
    view_center_s = 0.5f;
    view_center_t = 0.5f;
    if (!view_picture(is, &vp)) {
        return 0.0f;
    }

    return view_measure(is, vp, NULL, NULL);
}

void view_pan_by(VideoState *is, float dx, float dy) {
    SDL_Rect rect, clip;
    Frame *vp;
    float max_pan_x, max_pan_y, pan_x, pan_y;

    if (!view_picture(is, &vp)) {
        return;
    }
    view_measure(is, vp, &rect, &clip);

    view_pan_limits(rect.w, rect.h, clip.w, clip.h, &max_pan_x, &max_pan_y);
    pan_x = av_clipf(display_pan_x + dx, -max_pan_x, max_pan_x);
    pan_y = av_clipf(display_pan_y + dy, -max_pan_y, max_pan_y);

    view_center_from_display(0.5f - pan_x / (float)FFMAX(rect.w, 1),
                             0.5f - pan_y / (float)FFMAX(rect.h, 1));
}
