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

/* clang-format off */
#include "lachesis_alloc.h"
#include "lachesis_config.h"
#include "lachesis_hwaccel.h"
#include "lachesis_log.h"
#include "lachesis_renderer.h"
#include "lachesis_renderer_internal.h"
/* clang-format on */

#include <stdbool.h>
#include <stddef.h>

#include <libplacebo/utils/libav.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#define LACHESIS_ZERO_COPY_STRIKES 2

static void zero_copy_note_failed(RendererContext *ctx) {
    ctx->zero_copy_failed = 1;
    ctx->zero_copy_failed_pool = ctx->zero_copy_pool;
}

#if LACHESIS_HAVE_VULKAN

static int create_hw_frame(Renderer *renderer, AVFrame *frame) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWFramesContext *src_hw_frame = (AVHWFramesContext *)
                                          frame->hw_frames_ctx->data;
    AVHWFramesContext *hw_frame;
    AVVulkanFramesContext *vk_frame_ctx;
    int ret;

    if (ctx->hw_frame_ref) {
        hw_frame = (AVHWFramesContext *)ctx->hw_frame_ref->data;

        if (hw_frame->width == frame->width &&
            hw_frame->height == frame->height &&
            hw_frame->sw_format == src_hw_frame->sw_format) {
            return 0;
        }

        av_buffer_unref(&ctx->hw_frame_ref);
        av_freep(&ctx->transfer_formats);
    }

    if (!ctx->constraints) {
        ctx->constraints = av_hwdevice_get_hwframe_constraints(
            ctx->hw_device_ref, NULL);
        if (!ctx->constraints) {
            return AVERROR(ENOMEM);
        }
    }

    if ((ctx->constraints->max_width &&
         ctx->constraints->max_width < frame->width) ||
        (ctx->constraints->max_height &&
         ctx->constraints->max_height < frame->height) ||
        (ctx->constraints->min_width &&
         ctx->constraints->min_width > frame->width) ||
        (ctx->constraints->min_height &&
         ctx->constraints->min_height > frame->height)) {
        return 0;
    }

    if (ctx->constraints->valid_sw_formats) {
        enum AVPixelFormat *sw_formats = ctx->constraints->valid_sw_formats;
        while (*sw_formats != AV_PIX_FMT_NONE) {
            if (*sw_formats == src_hw_frame->sw_format) {
                break;
            }
            sw_formats++;
        }
        if (*sw_formats == AV_PIX_FMT_NONE) {
            return 0;
        }
    }

    ctx->hw_frame_ref = av_hwframe_ctx_alloc(ctx->hw_device_ref);
    if (!ctx->hw_frame_ref) {
        return AVERROR(ENOMEM);
    }

    hw_frame = (AVHWFramesContext *)ctx->hw_frame_ref->data;
    hw_frame->format = AV_PIX_FMT_VULKAN;
    hw_frame->sw_format = src_hw_frame->sw_format;
    hw_frame->width = frame->width;
    hw_frame->height = frame->height;

    if (frame->format == AV_PIX_FMT_CUDA ||
        frame->format == AV_PIX_FMT_VAAPI) {
        vk_frame_ctx = hw_frame->hwctx;
        vk_frame_ctx->flags = AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
    }

    ret = av_hwframe_ctx_init(ctx->hw_frame_ref);
    if (ret < 0) {
        av_buffer_unref(&ctx->hw_frame_ref);
        return ret;
    }

    /* Make sure the view usage doesn't exceed the real image usage. */
    if (frame->format == AV_PIX_FMT_VAAPI) {
        vk_frame_ctx = hw_frame->hwctx;
        vk_frame_ctx->usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    av_freep(&ctx->transfer_formats);
    if (av_hwframe_transfer_get_formats(ctx->hw_frame_ref,
                                        AV_HWFRAME_TRANSFER_DIRECTION_TO,
                                        &ctx->transfer_formats, 0) < 0) {
        av_freep(&ctx->transfer_formats);
    }

    return 0;
}

static inline int check_hw_transfer(RendererContext *ctx, AVFrame *frame) {
    if (!ctx->hw_frame_ref || !ctx->transfer_formats) {
        return 0;
    }

    for (int i = 0; ctx->transfer_formats[i] != AV_PIX_FMT_NONE; i++) {
        if (ctx->transfer_formats[i] == frame->format) {
            return 1;
        }
    }

    return 0;
}

static inline int move_to_output_frame(RendererContext *ctx, AVFrame *frame) {
    int ret;

    if (ctx->vk_frame->width < frame->width ||
        ctx->vk_frame->height < frame->height) {
        return AVERROR_INVALIDDATA;
    }

    ret = av_frame_copy_props(ctx->vk_frame, frame);
    if (ret < 0) {
        return ret;
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->vk_frame);
    return 0;
}

static int map_frame(Renderer *renderer, AVFrame *frame, int use_hw_frame) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (use_hw_frame && !ctx->hw_frame_ref) {
        return AVERROR(ENOSYS);
    }

    av_frame_unref(ctx->vk_frame);
    if (use_hw_frame) {
        ctx->vk_frame->hw_frames_ctx = av_buffer_ref(ctx->hw_frame_ref);
        ctx->vk_frame->format = AV_PIX_FMT_VULKAN;
    }
    ret = av_hwframe_map(ctx->vk_frame, frame, AV_HWFRAME_MAP_READ);
    if (!ret) {
        return move_to_output_frame(ctx, frame);
    }

    return ret;
}

static int transfer_frame(Renderer *renderer, AVFrame *frame, int use_hw_frame) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (use_hw_frame && !check_hw_transfer(ctx, frame)) {
        return AVERROR(ENOSYS);
    }

    av_frame_unref(ctx->vk_frame);
    if (use_hw_frame) {
        av_hwframe_get_buffer(ctx->hw_frame_ref, ctx->vk_frame, 0);
    }
    ret = av_hwframe_transfer_data(ctx->vk_frame, frame, 1);
    if (!ret) {
        return move_to_output_frame(ctx, frame);
    }

    return ret;
}

static int convert_frame_vulkan(Renderer *renderer, AVFrame *frame) {
    RendererContext *ctx = (RendererContext *)renderer;
    static int warned_download;
    int ret = AVERROR(ENOSYS);

    if (frame->format == AV_PIX_FMT_VULKAN) {
        return 0;
    }

    create_hw_frame(renderer, frame);

    for (int use_hw = !ctx->zero_copy_failed; use_hw >= 0; use_hw--) {
        const char *how = "mapping";

        ret = map_frame(renderer, frame, use_hw);
        if (ret) {
            ret = transfer_frame(renderer, frame, use_hw);
            how = "copy";
        }
        if (!ret) {
            if (use_hw) {
                ctx->zero_copy_misses = 0;
            } else if (!warned_download) {
                warned_download = 1;
                log_info("Displaying hardware frames via a system memory %s.\n",
                         how);
            }
            return 0;
        }
        if (use_hw &&
            ++ctx->zero_copy_misses >= LACHESIS_ZERO_COPY_STRIKES) {
            zero_copy_note_failed(ctx);
        }
    }

    return ret;
}

#endif /* LACHESIS_HAVE_VULKAN */

static int convert_frame_readback(RendererContext *ctx, AVFrame *frame) {
    static int warned_download;
    int ret;

    if (!ctx->sw_frame) {
        ctx->sw_frame = av_frame_alloc();
        if (!ctx->sw_frame) {
            return AVERROR(ENOMEM);
        }
    }

    ret = hwdownload_frame(&ctx->readback, ctx->sw_frame, frame);
    if (ret < 0) {
        return ret;
    }

    if (!warned_download) {
        warned_download = 1;
        log_info("Displaying hardware frames via a system memory copy: the %s "
                 "renderer cannot import %s.\n",
                 renderer_api_name(&ctx->api),
                 av_get_pix_fmt_name(frame->format));
#if LACHESIS_HAVE_D3D11
        if (ctx->api.backend != RENDERER_API_D3D11 &&
            frame->format == AV_PIX_FMT_D3D11) {
        }
#endif
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->sw_frame);

    return 0;
}

static void zero_copy_give_up(RendererContext *ctx, const AVFrame *frame) {
    static int warned;

    zero_copy_note_failed(ctx);

    if (!warned) {
        warned = 1;
        log_warn("The GPU rejected a zero copy import of a %s frame. Falling "
                 "back to a system memory copy.\n",
                 av_get_pix_fmt_name(frame->format));
    }
}

int convert_frame(Renderer *renderer, AVFrame *frame) {
    RendererContext *ctx = (RendererContext *)renderer;
    const AVHWFramesContext *hwfc;

    if (!frame->hw_frames_ctx) {
        return 0;
    }

    hwfc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    ctx->zero_copy_pool = (struct ZeroCopyPool){
        .sw_format = hwfc->sw_format,
        .width = hwfc->width,
        .height = hwfc->height,
    };

    if (ctx->zero_copy_failed &&
        (ctx->zero_copy_failed_pool.sw_format != ctx->zero_copy_pool.sw_format ||
         ctx->zero_copy_failed_pool.width != ctx->zero_copy_pool.width ||
         ctx->zero_copy_failed_pool.height != ctx->zero_copy_pool.height)) {
        ctx->zero_copy_failed = 0;
        ctx->zero_copy_misses = 0;
    }

#if LACHESIS_HAVE_VULKAN
    if (renderer->backend == RENDERER_API_VULKAN) {
        return convert_frame_vulkan(renderer, frame);
    }
#endif

#if LACHESIS_HAVE_D3D11
    if (renderer->backend == RENDERER_API_D3D11 &&
        frame->format == AV_PIX_FMT_D3D11 && !ctx->zero_copy_failed) {
        return 0;
    }
#endif

    if (!ctx->zero_copy_failed && pl_test_pixfmt(ctx->gpu, frame->format)) {
        return 0;
    }

    return convert_frame_readback(ctx, frame);
}

bool map_avframe_tex(RendererContext *ctx, AVFrame *frame, pl_tex *tex,
                     struct pl_frame *out) {
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11 &&
        frame->format == AV_PIX_FMT_D3D11) {
        if (map_d3d11_frame(ctx, frame, out)) {
            return true;
        }
        zero_copy_give_up(ctx, frame);
        if (convert_frame_readback(ctx, frame) < 0) {
            return false;
        }
    }
#endif

    if (pl_map_avframe_ex(ctx->gpu, out,
                          pl_avframe_params(.frame = frame, .tex = tex))) {
        return true;
    }

    if (!frame->hw_frames_ctx) {
        return false;
    }
    zero_copy_give_up(ctx, frame);
    if (convert_frame_readback(ctx, frame) < 0) {
        return false;
    }

    return pl_map_avframe_ex(ctx->gpu, out,
                             pl_avframe_params(.frame = frame, .tex = tex));
}

bool map_video_frame(RendererContext *ctx, AVFrame *frame,
                     struct pl_frame *out) {
    return map_avframe_tex(ctx, frame, ctx->tex, out);
}

static bool frames_alias(const AVFrame *a, const AVFrame *b) {
    if (!a || !b) {
        return false;
    }
    if (a == b) {
        return true;
    }
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (a->buf[i] && b->buf[i] && a->buf[i]->buffer == b->buf[i]->buffer) {
            return true;
        }
    }
    if (a->buf[0] && b->buf[0]) {
        return false;
    }
    return a->data[0] && a->data[0] == b->data[0];
}

const struct pl_frame *map_deint_ref(RendererContext *ctx, pl_tex *tex,
                                     struct pl_frame *out,
                                     const struct pl_frame *cur, AVFrame *frame,
                                     const AVFrame *self) {
    if (!frame || frame->width <= 0 || frames_alias(frame, self)) {
        return NULL;
    }
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11 &&
        frame->format == AV_PIX_FMT_D3D11) {
        if (!map_d3d11_frame(ctx, frame, out)) {
            return NULL;
        }
    } else
#endif
        if (!pl_map_avframe_ex(ctx->gpu, out,
                               pl_avframe_params(.frame = frame, .tex = tex))) {
        return NULL;
    }
    if (out->num_planes != cur->num_planes) {
        goto reject;
    }
    for (int i = 0; i < cur->num_planes; i++) {
        const struct pl_tex_params *a, *b;
        if (!cur->planes[i].texture || !out->planes[i].texture) {
            goto reject;
        }
        a = &cur->planes[i].texture->params;
        b = &out->planes[i].texture->params;
        if (a->w != b->w || a->h != b->h ||
            a->format->num_components != b->format->num_components) {
            goto reject;
        }
    }

    return out;

reject:
    pl_unmap_avframe(ctx->gpu, out);
    return NULL;
}
