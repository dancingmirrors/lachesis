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

#include <libavutil/frame.h>

#include "lachesis_view360.h"

typedef struct VkRenderer VkRenderer;

#define VIDEO_BACKGROUND_TILE_SIZE 64

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

enum DeinterlaceMode {
    DEINTERLACE_OFF = 0,
    DEINTERLACE_YADIF,
    DEINTERLACE_BOB,
    DEINTERLACE_MODE_COUNT,
};

typedef struct RenderParams {
    SDL_Rect target_rect;
    uint8_t video_background_color[4];
    enum VideoBackgroundType video_background_type;
    void *osd_pixels;
    int osd_width;
    int osd_height;
    int osd_stride;
    int disable_linear_scaling;
    int skip_anti_aliasing;
    int deinterlace;
    int rotate;
} RenderParams;

#define VK_DECODE_CAP_H264 (1u << 0)
#define VK_DECODE_CAP_HEVC (1u << 1)
#define VK_DECODE_CAP_AV1 (1u << 2)
#define VK_DECODE_CAP_VP9 (1u << 3)

VkRenderer *vk_get_renderer(void);

unsigned vk_renderer_video_decode_caps(VkRenderer *renderer);

/* Safe to call with a NULL renderer. */
int vk_renderer_frame_stats(VkRenderer *renderer, double *acquire_ms,
                            double *render_ms, double *present_ms);

int vk_renderer_create(VkRenderer *renderer, SDL_Window *window,
                       AVDictionary *opt);

int vk_renderer_get_hw_dev(VkRenderer *renderer, AVBufferRef **dev);

int vk_renderer_display(VkRenderer *renderer, AVFrame *frame, RenderParams *params);

int vk_renderer_capture(VkRenderer *renderer, AVFrame *frame, RenderParams *params,
                        int width, int height, uint8_t *out, int out_stride);

int vk_renderer_resize(VkRenderer *renderer, int width, int height);

int vk_renderer_self_test(VkRenderer *renderer, int width, int height);

void vk_renderer_destroy(VkRenderer *renderer);

int vk_renderer_enable_360(VkRenderer *renderer, enum View360Layout layout);
void vk_renderer_update_360(VkRenderer *renderer, float yaw, float pitch, float roll, float hfov);

#endif /* LACHESIS_RENDERER_H */
