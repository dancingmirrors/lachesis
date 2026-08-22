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

#ifndef LACHESIS_RENDERER_H
#define LACHESIS_RENDERER_H

#include <SDL3/SDL.h>

#include <libavutil/dict.h>
#include <libavutil/frame.h>

#include "lachesis_supersample.h"
#include "lachesis_view360.h"

typedef struct Renderer Renderer;

enum RendererApi {
    RENDERER_API_AUTO,
    RENDERER_API_VULKAN,
    RENDERER_API_OPENGL,
    RENDERER_API_D3D11,
};

#define VIDEO_BACKGROUND_TILE_SIZE 64

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

#define LACHESIS_MAX_MIX_FRAMES 2

typedef struct RenderMixFrame {
    AVFrame *frame;
    uint64_t signature;
    float ts;
} RenderMixFrame;

typedef struct RenderParams {
    SDL_Rect target_rect;
    uint8_t video_background_color[4];
    enum VideoBackgroundType video_background_type;
    int video_background_explicit;
    void *osd_pixels;
    int osd_width;
    int osd_height;
    int osd_stride;
    int osd_x;
    int osd_y;
    unsigned osd_generation;
    void *sub_pixels;
    int sub_width;
    int sub_height;
    int sub_stride;
    unsigned sub_generation;
    void *text_sub_pixels;
    int text_sub_width;
    int text_sub_height;
    int text_sub_stride;
    int text_sub_x;
    int text_sub_y;
    unsigned text_sub_generation;
    int still_image;
    int disable_linear_scaling;
    int skip_anti_aliasing;
    int cheapest;
    int deinterlace;
    int second_field;
    AVFrame *prev_frame;
    AVFrame *next_frame;
    const RenderMixFrame *mix_frames;
    int mix_num_frames;
    float mix_vsync_duration;
    int rotate;
    int eq_brightness;
    int eq_gamma;
    int eq_contrast;
    int eq_saturation;
    int64_t present_done_us;
    int64_t present_block_us;
    int present_source;
    int64_t present_display_us;
    double present_refresh_us;
} RenderParams;

#define RENDERER_DECODE_CAP_H264 (1u << 0)
#define RENDERER_DECODE_CAP_HEVC (1u << 1)
#define RENDERER_DECODE_CAP_AV1 (1u << 2)
#define RENDERER_DECODE_CAP_VP9 (1u << 3)

typedef struct RendererOpenParams {
    const char *title;
    int width;
    int height;
    Uint32 window_flags;
    enum RendererApi api;
    unsigned exclude;
    AVDictionary *opt;
} RendererOpenParams;

int renderer_open(const RendererOpenParams *params, SDL_Window **window,
                  Renderer **out, char *why, size_t why_size);

enum RendererApi renderer_api(const Renderer *renderer);
const char *renderer_api_name(const Renderer *renderer);
const char *renderer_device_name(const Renderer *renderer);

unsigned renderer_video_decode_caps(Renderer *renderer);

const enum AVPixelFormat *renderer_supported_pixfmts(Renderer *renderer,
                                                     int *count);

/* Safe to call with a NULL renderer. */
int renderer_max_texture_size(Renderer *renderer);

int renderer_is_vsync_blocked(Renderer *renderer);

/* Safe to call with a NULL renderer. */
int renderer_frame_stats(Renderer *renderer, double *acquire_ms,
                         double *render_ms, double *present_ms);

/* NULL unless the backend can decode into its own memory. */
int renderer_get_hw_dev(Renderer *renderer, AVBufferRef **dev);

/* Call only from the event loop. Safe to call with a NULL renderer. */
int renderer_take_image_repaint(Renderer *renderer);

int renderer_display(Renderer *renderer, AVFrame *frame, RenderParams *params);
int renderer_display_blank(Renderer *renderer, RenderParams *params);

int renderer_capture(Renderer *renderer, AVFrame *frame, RenderParams *params,
                     int width, int height, uint8_t *out, int out_stride);

int renderer_resize(Renderer *renderer, int width, int height);

/* Safe to call with a NULL renderer. */
int renderer_refresh_display_info(Renderer *renderer, SDL_Window *window);

void renderer_destroy(Renderer *renderer);

int renderer_enable_360(Renderer *renderer, enum View360Layout layout,
                        enum View360Projection projection);
void renderer_update_360(Renderer *renderer, float yaw, float pitch, float roll, float hfov);

int renderer_set_supersample(Renderer *renderer, enum SupersampleLevel level);

#endif /* LACHESIS_RENDERER_H */
