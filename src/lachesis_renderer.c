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
#include "lachesis_cache.h"
#include "lachesis_config.h"
#include "lachesis_deinterlace.h"
#include "lachesis_equalizer.h"
#include "lachesis_hwaccel.h"
#include "lachesis_icc.h"
#include "lachesis_icon.h"
#include "lachesis_log.h"
#include "lachesis_present.h"
#include "lachesis_renderer.h"
#include "lachesis_renderer_internal.h"
#include "lachesis_scale.h"
#include "lachesis_supersample.h"
#include "lachesis_view360.h"
/* clang-format on */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <libplacebo/config.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/deinterlacing.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/libav.h>

#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/buffer.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/version.h>

#define LACHESIS_MAX_OVERLAYS 3
#define LACHESIS_MAX_HOOKS 2

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#elif defined(__linux__) || defined(__GLIBC__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <link.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#endif

int renderer_allow_software_gpu = 1;
int renderer_want_translucent;
const char *renderer_want_device;

static void vo_state_lock(RendererContext *ctx) {
    if (ctx->vo.lock) {
        SDL_LockMutex(ctx->vo.lock);
    }
}

static void vo_state_unlock(RendererContext *ctx) {
    if (ctx->vo.lock) {
        SDL_UnlockMutex(ctx->vo.lock);
    }
}

enum GpuClass renderer_gpu_class_request(const char *want) {
    static const struct {
        const char *name;
        enum GpuClass want;
    } aliases[] = {
        {"discrete", GPU_CLASS_DISCRETE},
        {"dgpu", GPU_CLASS_DISCRETE},
        {"integrated", GPU_CLASS_INTEGRATED},
        {"igpu", GPU_CLASS_INTEGRATED},
    };

    for (size_t i = 0; want && i < FF_ARRAY_ELEMS(aliases); i++) {
        if (!av_strcasecmp(want, aliases[i].name)) {
            return aliases[i].want;
        }
    }

    return GPU_CLASS_ANY;
}

void renderer_report_gpu_devices(const char *api, const GpuDeviceNames names,
                                 int num, int verbose) {
    void (*say)(const char *, ...) = verbose ? log_verbose : log_info;

    if (!num) {
        say("No %s devices are available.\n", api);
        return;
    }
    say("Available %s devices:\n", api);
    for (int i = 0; i < num; i++) {
        say("  %s\n", names[i]);
    }
}

int renderer_match_gpu_device(const GpuDeviceNames names,
                              const enum GpuClass *classes, int num,
                              const char *want) {
    enum GpuClass wanted = renderer_gpu_class_request(want);
    int wild = strchr(want, '*') || strchr(want, '?');
    char anywhere[300];

    if (wanted != GPU_CLASS_ANY) {
        for (int i = 0; classes && i < num; i++) {
            if (classes[i] == wanted) {
                return i;
            }
        }
        return -1;
    }
    for (int i = 0; i < num; i++) {
        if (!strcmp(names[i], want)) {
            return i;
        }
    }
    for (int i = 0; i < num; i++) {
        if (wild ? hwaccel_glob_match(want, names[i])
                 : av_stristr(names[i], want) != NULL) {
            return i;
        }
    }
    if (!wild) {
        return -1;
    }

    snprintf(anywhere, sizeof(anywhere), "*%s*", want);
    for (int i = 0; i < num; i++) {
        if (hwaccel_glob_match(anywhere, names[i])) {
            return i;
        }
    }

    return -1;
}

#ifdef LACHESIS_CAN_ITERATE_LIBS
#define LACHESIS_MAX_PLACEBO_LIBS 8

static long libplacebo_soversion(const char *path) {
    const char *base;
    const char *stem;

    if (!path || !*path) {
        return -1;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    stem = strstr(base, "libplacebo");
    if (!stem) {
        return -1;
    }
    stem += strlen("libplacebo");

    for (; *stem; stem++) {
        if (*stem >= '0' && *stem <= '9') {
            return strtol(stem, NULL, 10);
        }
    }

    return 0;
}

struct placebo_lib_scan {
    const char *paths[LACHESIS_MAX_PLACEBO_LIBS];
    long versions[LACHESIS_MAX_PLACEBO_LIBS];
    int count;
};

static void placebo_lib_scan_add(struct placebo_lib_scan *scan,
                                 const char *path, long version) {
    for (int i = 0; i < scan->count; i++) {
        if (scan->versions[i] == version) {
            return;
        }
    }
    if (scan->count < LACHESIS_MAX_PLACEBO_LIBS) {
        scan->paths[scan->count] = path;
        scan->versions[scan->count] = version;
        scan->count++;
    }
}

#ifndef __APPLE__
static int placebo_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    struct placebo_lib_scan *scan = data;
    long version;
    (void)size;

    version = libplacebo_soversion(info->dlpi_name);
    if (version >= 0) {
        placebo_lib_scan_add(scan, info->dlpi_name, version);
    }
    return 0;
}
#endif

static void placebo_scan_loaded_libs(struct placebo_lib_scan *scan) {
    scan->count = 0;
#ifdef __APPLE__
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        long version = libplacebo_soversion(name);
        if (version >= 0) {
            placebo_lib_scan_add(scan, name, version);
        }
    }
#else
    dl_iterate_phdr(placebo_phdr_cb, scan);
#endif
}
#endif /* LACHESIS_CAN_ITERATE_LIBS */

static void check_libplacebo_consistency(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;

#ifdef LACHESIS_CAN_ITERATE_LIBS
    struct placebo_lib_scan scan;
    placebo_scan_loaded_libs(&scan);

    if (scan.count > 1) {
        log_warn("Multiple libplacebo versions are loaded into this process.\n");
    }

    if (scan.count == 1 && scan.versions[0] > 0 &&
        scan.versions[0] != PL_API_VER) {
        log_warn("PL_API_VER mismatch detected.\n");
    }
#endif /* LACHESIS_CAN_ITERATE_LIBS */
}

static int build_pixfmt_list(RendererContext *ctx) {
    const AVPixFmtDescriptor *desc = NULL;
    int n = 0, cap = 0;

    while ((desc = av_pix_fmt_desc_next(desc))) {
        enum AVPixelFormat fmt = av_pix_fmt_desc_get_id(desc);

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) &&
            !pl_test_pixfmt(ctx->gpu, fmt)) {
            continue;
        }

        if (n + 1 >= cap) {
            enum AVPixelFormat *grown;

            cap = cap ? cap * 2 : 64;
            grown = av_realloc_array(ctx->pixfmts, cap, sizeof(*grown));
            if (!grown) {
                return AVERROR(ENOMEM);
            }
            ctx->pixfmts = grown;
        }
        ctx->pixfmts[n++] = fmt;
    }

    if (!n) {
        return AVERROR_EXTERNAL;
    }
    ctx->num_pixfmts = n;

    return 0;
}

static void vk_log_cb(void *log_priv, enum pl_log_level level,
                      const char *msg) {
    (void)log_priv;

    if (level <= PL_LOG_WARN) {
        log_warn("libplacebo: %s\n", msg);
    } else {
        log_verbose("libplacebo: %s\n", msg);
    }
}

static int create(Renderer *renderer, SDL_Window *window, AVDictionary *opt) {
    struct pl_log_params log_params = {
        .log_cb = vk_log_cb,
        /* Not PL_LOG_WARN due to useless spam. */
        .log_level = enable_debug(opt) ? PL_LOG_DEBUG : PL_LOG_ERR,
        .log_priv = renderer,
    };
    RendererContext *ctx = (RendererContext *)renderer;
    const char *backend = renderer->backend == RENDERER_API_OPENGL ? "OpenGL"
        : renderer->backend == RENDERER_API_D3D11                  ? "D3D11"
                                                                   : "Vulkan";
    AVDictionaryEntry *entry;
    int ret;

    check_libplacebo_consistency();

    ctx->log_ctx = pl_log_create(PL_API_VER, &log_params);
    if (!ctx->log_ctx) {
        return AVERROR(ENOMEM);
    }

    entry = av_dict_get(opt, "benchmark", NULL, 0);
    ctx->benchmark = entry && strtol(entry->value, NULL, 10);

    switch (renderer->backend) {
#if LACHESIS_HAVE_VULKAN
    case RENDERER_API_VULKAN:
        ret = vk_backend_create(ctx, window, opt);
        break;
#endif
#if LACHESIS_HAVE_OPENGL
    case RENDERER_API_OPENGL:
        ret = gl_backend_create(ctx, window, opt);
        break;
#endif
#if LACHESIS_HAVE_D3D11
    case RENDERER_API_D3D11:
        ret = d3d11_backend_create(ctx, window, opt);
        break;
#endif
    default:
        ret = AVERROR(ENOSYS);
        break;
    }
    if (ret < 0) {
        return ret;
    }

    shader_cache_open(&ctx->shader_cache, ctx->gpu, ctx->log_ctx, backend, opt);

    icc_setup(ctx, window, opt);

    entry = av_dict_get(opt, "display_hdr", NULL, 0);
    ctx->hdr_auto = !entry || strtol(entry->value, NULL, 10);
    if (renderer->backend == RENDERER_API_OPENGL) {
        ctx->hdr_auto = 0;
    }
    hdr_refresh(ctx, window);

    ctx->renderer = pl_renderer_create(ctx->log_ctx, ctx->gpu);
    if (!ctx->renderer) {
        return AVERROR_EXTERNAL;
    }

    ret = build_pixfmt_list(ctx);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

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

static int convert_frame(Renderer *renderer, AVFrame *frame) {
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

static bool map_avframe_tex(RendererContext *ctx, AVFrame *frame, pl_tex *tex,
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

static bool map_video_frame(RendererContext *ctx, AVFrame *frame,
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

static const struct pl_frame *map_deint_ref(RendererContext *ctx, pl_tex *tex,
                                            struct pl_frame *out,
                                            const struct pl_frame *cur,
                                            AVFrame *frame, const AVFrame *self) {
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

static int visible_target_rect(const struct pl_frame *target,
                               const SDL_Rect *clip, pl_rect2df *out) {
    pl_tex tex = target->num_planes > 0 ? target->planes[0].texture : NULL;
    const pl_rect2df *dst = &target->crop;
    pl_rect2df vis;

    if (!tex) {
        return 0;
    }
    vis = (pl_rect2df){
        .x0 = FFMAX(dst->x0, 0.0f),
        .y0 = FFMAX(dst->y0, 0.0f),
        .x1 = FFMIN(dst->x1, (float)tex->params.w),
        .y1 = FFMIN(dst->y1, (float)tex->params.h),
    };
    if (clip && clip->w > 0 && clip->h > 0) {
        vis.x0 = FFMAX(vis.x0, (float)clip->x);
        vis.y0 = FFMAX(vis.y0, (float)clip->y);
        vis.x1 = FFMIN(vis.x1, (float)(clip->x + clip->w));
        vis.y1 = FFMIN(vis.y1, (float)(clip->y + clip->h));
    }
    if (vis.x1 <= vis.x0 || vis.y1 <= vis.y0) {
        return 0;
    }

    *out = vis;

    return 1;
}

static void clip_crops_to_target(struct pl_frame *image, struct pl_frame *target,
                                 pl_rotation rotation, const SDL_Rect *clip) {
    pl_rect2df *dst = &target->crop;
    pl_rect2df *src = &image->crop;
    float dst_w = dst->x1 - dst->x0, dst_h = dst->y1 - dst->y0;
    float src_w = src->x1 - src->x0, src_h = src->y1 - src->y0;
    float sx = src->x0, sy = src->y0;
    float u0, u1, v0, v1;
    float p0, p1, q0, q1;
    pl_rect2df vis;

    if (dst_w <= 0 || dst_h <= 0 || src_w == 0 || src_h == 0) {
        return;
    }
    if (!visible_target_rect(target, clip, &vis)) {
        return;
    }
    if (vis.x0 == dst->x0 && vis.y0 == dst->y0 &&
        vis.x1 == dst->x1 && vis.y1 == dst->y1) {
        return;
    }

    u0 = (vis.x0 - dst->x0) / dst_w;
    u1 = (vis.x1 - dst->x0) / dst_w;
    v0 = (vis.y0 - dst->y0) / dst_h;
    v1 = (vis.y1 - dst->y0) / dst_h;

    switch (rotation) {
    case PL_ROTATION_90:
        p0 = v0;
        p1 = v1;
        q0 = 1.0f - u1;
        q1 = 1.0f - u0;
        break;
    case PL_ROTATION_180:
        p0 = 1.0f - u1;
        p1 = 1.0f - u0;
        q0 = 1.0f - v1;
        q1 = 1.0f - v0;
        break;
    case PL_ROTATION_270:
        p0 = 1.0f - v1;
        p1 = 1.0f - v0;
        q0 = u0;
        q1 = u1;
        break;
    default:
        p0 = u0;
        p1 = u1;
        q0 = v0;
        q1 = v1;
        break;
    }

    *src = (pl_rect2df){
        .x0 = sx + p0 * src_w,
        .y0 = sy + q0 * src_h,
        .x1 = sx + p1 * src_w,
        .y1 = sy + q1 * src_h,
    };
    *dst = vis;
}

/* Theoretically needs consideration for fractional scaling. */
static View360Viewport clip_360_viewport(struct pl_frame *target,
                                         const SDL_Rect *clip) {
    View360Viewport viewport = VIEW360_VIEWPORT_WHOLE;
    pl_tex tex = target->num_planes > 0 ? target->planes[0].texture : NULL;
    pl_rect2df *dst = &target->crop;
    float w = dst->x1 - dst->x0;
    float h = dst->y1 - dst->y0;
    pl_rect2df visible;

    if (!tex || w <= 0.0f || h <= 0.0f) {
        return viewport;
    }
    viewport.aspect = w / h;

    if (!visible_target_rect(target, clip, &visible)) {
        return viewport;
    }

    viewport.off_x = (visible.x0 - dst->x0) / w;
    viewport.off_y = (visible.y0 - dst->y0) / h;
    viewport.scale_x = (visible.x1 - visible.x0) / w;
    viewport.scale_y = (visible.y1 - visible.y0) / h;
    *dst = visible;

    return viewport;
}

static pl_tex overlay_upload(RendererContext *ctx, pl_tex *slot, void *pixels,
                             int w, int h, int stride, unsigned generation,
                             unsigned *held) {
    if (*slot && (int)(*slot)->params.w == w && (int)(*slot)->params.h == h &&
        generation && *held == generation) {
        return *slot;
    }
    if (!*slot || (int)(*slot)->params.w != w || (int)(*slot)->params.h != h) {
        pl_fmt fmt = pl_find_named_fmt(ctx->gpu, "rgba8");
        pl_tex_destroy(ctx->gpu, slot);
        if (!fmt) {
            return NULL;
        }
        *slot = pl_tex_create(ctx->gpu, &(struct pl_tex_params){
                                            .w = w,
                                            .h = h,
                                            .format = fmt,
                                            .sampleable = true,
                                            .host_writable = true,
                                        });
        if (!*slot) {
            return NULL;
        }
    }

    if (!pl_tex_upload(ctx->gpu, &(struct pl_tex_transfer_params){
                                     .tex = *slot,
                                     .ptr = pixels,
                                     .row_pitch = stride,
                                 })) {
        *held = 0;
        return NULL;
    }
    *held = generation;

    return *slot;
}

static int supersample_active(const RendererContext *ctx,
                              const ImageState *image) {
    return ctx->supersample_level != SUPERSAMPLE_OFF && !ctx->benchmark &&
        !image->moving;
}

static void setup_render(RendererContext *ctx, struct pl_frame *pl_frame,
                         struct pl_frame *target, struct pl_render_params *pl_params,
                         RenderParams *params, struct pl_overlay *overlays,
                         struct pl_overlay_part *parts,
                         const struct pl_hook **hooks,
                         const ImageState *image) {
    SDL_Rect *rect = &params->target_rect;
    target->crop = (pl_rect2df){.x0 = rect->x, .x1 = rect->x + rect->w, .y0 = rect->y, .y1 = rect->y + rect->h};

    pl_rotation rotation = pl_rotation_normalize(params->rotate / 90);
    View360Viewport viewport = VIEW360_VIEWPORT_WHOLE;

    if (ctx->sbs360_enabled && ctx->sbs360_hook) {
        pl_frame->rotation = PL_ROTATION_0;
        viewport = clip_360_viewport(target, &params->target_clip);
    } else {
        pl_frame->rotation = rotation;
        clip_crops_to_target(pl_frame, target,
                             pl_rotation_normalize(rotation - target->rotation),
                             &params->target_clip);
    }
    int transparent = pl_frame->repr.alpha != PL_ALPHA_NONE;
    int border = params->video_background_explicit;

    switch (params->video_background_type) {
    case VIDEO_BACKGROUND_TILES:
        pl_params->tile_size = VIDEO_BACKGROUND_TILE_SIZE * 2;
        if (border) {
            pl_params->border = PL_CLEAR_TILES;
        }
        if (transparent) {
            pl_params->background = PL_CLEAR_TILES;
        }
        break;
    case VIDEO_BACKGROUND_COLOR:
        for (int i = 0; i < 3; i++) {
            pl_params->background_color[i] = params->video_background_color[i] / 255.0;
        }
        pl_params->background_transparency = (255 - params->video_background_color[3]) / 255.0;
        if (border) {
            pl_params->border = PL_CLEAR_COLOR;
        }
        if (transparent) {
            pl_params->background = PL_CLEAR_COLOR;
        }
        break;
    case VIDEO_BACKGROUND_NONE:
        if (transparent) {
            pl_frame->repr.alpha = PL_ALPHA_NONE;
        }
        break;
    }

    int num_hooks = 0;

    if (ctx->sbs360_enabled && ctx->sbs360_hook) {
        view360_pl_hook_update(ctx->sbs360_hook, ctx->sbs360_yaw,
                               ctx->sbs360_pitch, ctx->sbs360_roll,
                               ctx->sbs360_hfov, ctx->sbs360_layout,
                               ctx->sbs360_projection, (int)rotation * 90,
                               &viewport);
        hooks[num_hooks++] = ctx->sbs360_hook;
    }

    if (supersample_active(ctx, image) && ctx->supersample_hook) {
        supersample_pl_hook_update(ctx->supersample_hook, ctx->supersample_level);
        hooks[num_hooks++] = ctx->supersample_hook;
        pl_params->deband_params =
            supersample_deband_params(ctx->supersample_level);
    }

    if (num_hooks > 0) {
        pl_params->hooks = hooks;
        pl_params->num_hooks = num_hooks;
    }

    int num_overlays = 0;

    if (params->sub_pixels && params->sub_width > 0 && params->sub_height > 0) {
        pl_tex tex = overlay_upload(ctx, &ctx->sub_tex, params->sub_pixels,
                                    params->sub_width, params->sub_height,
                                    params->sub_stride, params->sub_generation,
                                    &ctx->sub_tex_generation);
        if (tex) {
            const SDL_Rect *at = params->target_plain.w > 0 &&
                    params->target_plain.h > 0
                ? &params->target_plain
                : rect;

            parts[num_overlays] = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->sub_width, .y1 = (float)params->sub_height},
                .dst = {.x0 = at->x, .y0 = at->y, .x1 = at->x + at->w, .y1 = at->y + at->h},
            };
            overlays[num_overlays] = (struct pl_overlay){
                .tex = tex,
                .mode = PL_OVERLAY_NORMAL,
                .coords = PL_OVERLAY_COORDS_DST_FRAME,
                .repr = {
                    .sys = PL_COLOR_SYSTEM_RGB,
                    .levels = PL_COLOR_LEVELS_FULL,
                    .alpha = PL_ALPHA_INDEPENDENT,
                },
                .color = pl_color_space_srgb,
                .parts = &parts[num_overlays],
                .num_parts = 1,
            };
            num_overlays++;
        }
    }

    if (params->text_sub_pixels && params->text_sub_width > 0 &&
        params->text_sub_height > 0) {
        pl_tex tex = overlay_upload(ctx, &ctx->text_sub_tex,
                                    params->text_sub_pixels,
                                    params->text_sub_width,
                                    params->text_sub_height,
                                    params->text_sub_stride,
                                    params->text_sub_generation,
                                    &ctx->text_sub_tex_generation);
        if (tex) {
            float x = (float)params->text_sub_x;
            float y = (float)params->text_sub_y;

            parts[num_overlays] = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->text_sub_width, .y1 = (float)params->text_sub_height},
                .dst = {.x0 = x, .y0 = y, .x1 = x + params->text_sub_width, .y1 = y + params->text_sub_height},
            };
            overlays[num_overlays] = (struct pl_overlay){
                .tex = tex,
                .mode = PL_OVERLAY_NORMAL,
                .coords = PL_OVERLAY_COORDS_DST_FRAME,
                .repr = {
                    .sys = PL_COLOR_SYSTEM_RGB,
                    .levels = PL_COLOR_LEVELS_FULL,
                    .alpha = PL_ALPHA_PREMULTIPLIED,
                },
                .color = pl_color_space_srgb,
                .parts = &parts[num_overlays],
                .num_parts = 1,
            };
            num_overlays++;
        }
    }

    if (params->osd_pixels && params->osd_width > 0 && params->osd_height > 0) {
        pl_tex tex = overlay_upload(ctx, &ctx->osd_tex, params->osd_pixels,
                                    params->osd_width, params->osd_height,
                                    params->osd_stride, params->osd_generation,
                                    &ctx->osd_tex_generation);
        if (tex) {
            float x = (float)params->osd_x;
            float y = (float)params->osd_y;

            parts[num_overlays] = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->osd_width, .y1 = (float)params->osd_height},
                .dst = {.x0 = x, .y0 = y, .x1 = x + params->osd_width, .y1 = y + params->osd_height},
            };
            overlays[num_overlays] = (struct pl_overlay){
                .tex = tex,
                .mode = PL_OVERLAY_NORMAL,
                .coords = PL_OVERLAY_COORDS_DST_FRAME,
                .repr = {
                    .sys = PL_COLOR_SYSTEM_RGB,
                    .levels = PL_COLOR_LEVELS_FULL,
                    .alpha = PL_ALPHA_PREMULTIPLIED,
                },
                .color = pl_color_space_srgb,
                .parts = &parts[num_overlays],
                .num_parts = 1,
            };
            num_overlays++;
        }
    }

    if (num_overlays > 0) {
        target->overlays = overlays;
        target->num_overlays = num_overlays;
    }
}

#define LACHESIS_STAT_EMA_FRAMES 30

#define LACHESIS_PRESENT_TIMING_GRACE 120

static void disable_present_timing(RendererContext *ctx) {
    switch (ctx->api.backend) {
#if LACHESIS_HAVE_VULKAN
    case RENDERER_API_VULKAN:
        vkpresent_disable();
        break;
#endif
#if LACHESIS_HAVE_D3D11
    case RENDERER_API_D3D11:
        d3dpresent_disable();
        break;
#endif
    default:
        break;
    }
}

static void collect_present_timing(RendererContext *ctx, RenderParams *params) {
    int source = PRESENT_SOURCE_SWAP;
    int polled = 0;

#if LACHESIS_HAVE_VULKAN
    if (ctx->api.backend == RENDERER_API_VULKAN) {
        VkPresentSample sample;

        source = vkpresent_source();
        if (source != PRESENT_SOURCE_SWAP && vkpresent_poll(&sample)) {
            source = sample.source;
            params->present_display_us = sample.display_us;
            params->present_refresh_us = sample.refresh_us;
            polled = 1;
        }
    }
#endif
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11) {
        D3DPresentSample sample;

        source = d3dpresent_source();
        if (source != PRESENT_SOURCE_SWAP && d3dpresent_poll(&sample)) {
            source = sample.source;
            params->present_display_us = sample.display_us;
            params->present_refresh_us = sample.refresh_us;
            polled = 1;
        }
    }
#endif

    params->present_source = source;
    if (source == PRESENT_SOURCE_SWAP) {
        return;
    }

    if (polled) {
        ctx->present_timing_silent = 0;
        return;
    }

    if (++ctx->present_timing_silent > LACHESIS_PRESENT_TIMING_GRACE) {
        disable_present_timing(ctx);
        params->present_source = PRESENT_SOURCE_SWAP;
    }
}

static void unmap_mix_slot(RendererContext *ctx, struct MixSlot *slot) {
    if (slot->mapped) {
        pl_unmap_avframe(ctx->gpu, &slot->frame);
        slot->mapped = 0;
    }
    slot->signature = 0;
    slot->used = 0;
}

static void release_mix_slots(RendererContext *ctx) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
        unmap_mix_slot(ctx, &ctx->mix_slots[i]);
    }
}

static void gpu_quiesce(RendererContext *ctx) {
    if (!ctx->gpu) {
        return;
    }
    pl_gpu_finish(ctx->gpu);
    release_mix_slots(ctx);
    pl_gpu_finish(ctx->gpu);
}

static void destroy_mix_slots(RendererContext *ctx) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
        struct MixSlot *slot = &ctx->mix_slots[i];

        unmap_mix_slot(ctx, slot);
        for (size_t j = 0; j < FF_ARRAY_ELEMS(slot->tex); j++) {
            pl_tex_destroy(ctx->gpu, &slot->tex[j]);
        }
    }
}

static const struct pl_frame *map_mix_frame(RendererContext *ctx,
                                            const RenderMixFrame *mix) {
    struct MixSlot *slot = NULL;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
        if (ctx->mix_slots[i].mapped &&
            ctx->mix_slots[i].signature == mix->signature) {
            ctx->mix_slots[i].used = 1;
#if LACHESIS_HAVE_D3D11
            if (ctx->api.backend == RENDERER_API_D3D11) {
                d3d11_touch_frame(ctx, &ctx->mix_slots[i].frame);
            }
#endif
            return &ctx->mix_slots[i].frame;
        }
    }

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
        if (!ctx->mix_slots[i].used) {
            slot = &ctx->mix_slots[i];
            unmap_mix_slot(ctx, slot);
            break;
        }
    }
    if (!slot) {
        return NULL;
    }

    if (convert_frame(&ctx->api, mix->frame) < 0 ||
        !map_avframe_tex(ctx, mix->frame, slot->tex, &slot->frame)) {
        return NULL;
    }

    slot->signature = mix->signature;
    slot->mapped = 1;
    slot->used = 1;

    return &slot->frame;
}

static int map_frame_mix(RendererContext *ctx, const AVFrame *frame,
                         const RenderParams *params, struct pl_frame *images,
                         const struct pl_frame **refs, float *timestamps,
                         uint64_t *signatures) {
    int num = 0;

    if (!params->mix_frames || params->mix_num_frames < 1 ||
        params->mix_num_frames > LACHESIS_MAX_MIX_FRAMES ||
        params->mix_vsync_duration <= 0.0f ||
        params->deinterlace) {
        return 0;
    }
    if (params->mix_frames[0].frame != frame) {
        return 0;
    }

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
        ctx->mix_slots[i].used = 0;
    }

    for (int i = 0; i < params->mix_num_frames; i++) {
        const struct pl_frame *mapped =
            map_mix_frame(ctx, &params->mix_frames[i]);

        if (!mapped) {
            break;
        }
        images[num] = *mapped;
        refs[num] = &images[num];
        timestamps[num] = params->mix_frames[i].ts;
        signatures[num] = params->mix_frames[i].signature;
        num++;
    }

    return num;
}

static bool can_sample_polar(const RendererContext *ctx) {
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11) {
        return ctx->gpu->glsl.compute;
    }
#endif
    (void)ctx;

    return true;
}

static const struct pl_filter_config *pick_scaler(const RendererContext *ctx,
                                                  enum pl_filter_usage usage) {
    const struct pl_filter_config *config = scale_filter();

    if (!config || ctx->benchmark) {
        return NULL;
    }
    if (!(config->allowed & usage)) {
        return NULL;
    }
    if (config->polar && !can_sample_polar(ctx)) {
        return &pl_filter_lanczos;
    }

    return config;
}

static const struct pl_filter_config *pick_downscaler(const RendererContext *ctx,
                                                      const RenderParams *params,
                                                      const ImageState *image) {
    const struct pl_filter_config *config;

    if (image->moving) {
        return NULL;
    }

    config = pick_scaler(ctx, PL_FILTER_DOWNSCALING);
    if (config) {
        return config;
    }

    if (ctx->benchmark || !params->still_image) {
        return &pl_filter_bilinear;
    }

    return &pl_filter_catmull_rom;
}

static const struct pl_filter_config *pick_upscaler(const RendererContext *ctx,
                                                    const ImageState *image) {
    const struct pl_filter_config *config;

    if (image->moving) {
        return NULL;
    }

    config = pick_scaler(ctx, PL_FILTER_UPSCALING);
    if (!config && supersample_active(ctx, image)) {
        config = supersample_upscaler(ctx->supersample_level);
        if (config && config->polar && !can_sample_polar(ctx)) {
            config = &pl_filter_lanczos;
        }
    }

    return config ? config : &pl_filter_bilinear;
}

static struct pl_color_adjustment equalizer_adjustment(const RenderParams *params) {
    struct pl_color_adjustment adj = {PL_COLOR_ADJUSTMENT_NEUTRAL};

    adj.brightness = equalizer_pl_brightness(params->eq_brightness);
    adj.gamma = equalizer_pl_gamma(params->eq_gamma);
    adj.contrast = equalizer_pl_contrast(params->eq_contrast);
    adj.saturation = equalizer_pl_saturation(params->eq_saturation);

    return adj;
}

#define LACHESIS_SWAPCHAIN_RETRY_GRACE 4

static int swapchain_sync_size(RendererContext *ctx) {
    int want_w, want_h;
    int w, h;
    int retry;

    vo_state_lock(ctx);
    if (!ctx->swapchain_stale) {
        vo_state_unlock(ctx);
        return 0;
    }
    w = want_w = ctx->swapchain_stale_w;
    h = want_h = ctx->swapchain_stale_h;
    vo_state_unlock(ctx);

    if (pl_swapchain_resize(ctx->swapchain, &w, &h)) {
        vo_state_lock(ctx);
        if (ctx->swapchain_stale_w == want_w && ctx->swapchain_stale_h == want_h) {
            ctx->swapchain_stale = 0;
            ctx->swapchain_retry = 0;
        }
        vo_state_unlock(ctx);
        ctx->present_timing_silent = 0;
        return 0;
    }

    vo_state_lock(ctx);
    retry = ++ctx->swapchain_retry;
    vo_state_unlock(ctx);
    if (retry < LACHESIS_SWAPCHAIN_RETRY_GRACE) {
        return 0;
    }
    if (retry == LACHESIS_SWAPCHAIN_RETRY_GRACE) {
        log_warn("The swapchain will not resize to %dx%d.\n", want_w, want_h);
    }

    return AVERROR_EXTERNAL;
}

static struct pl_render_params base_render_params(const RendererContext *ctx,
                                                  const RenderParams *params,
                                                  const ImageState *image,
                                                  struct pl_color_adjustment *adjustment) {
    return (struct pl_render_params){
        .upscaler = pick_upscaler(ctx, image),
        .downscaler = pick_downscaler(ctx, params, image),
        .color_adjustment = adjustment,
        .sigmoid_params =
            ctx->benchmark ? NULL : pl_render_default_params.sigmoid_params,
        .dither_params =
            ctx->benchmark ? NULL : pl_render_default_params.dither_params,
        .cone_params = pl_render_default_params.cone_params,
        .color_map_params = pl_render_default_params.color_map_params,
        .disable_linear_scaling = ctx->benchmark,
        .skip_anti_aliasing = ctx->benchmark,
    };
}

#define IMAGE_SETTLE_US 250000

static ImageState track_image(RendererContext *ctx, const RenderParams *params) {
    ImageTracker *t = &ctx->image;
    int64_t now = av_gettime_relative();
    ImageState image = {
        .rect = params->target_rect,
        .rotate = params->rotate,
        .moving = t->last.moving,
    };

    image.changed = t->last.rect.x != image.rect.x ||
        t->last.rect.y != image.rect.y ||
        t->last.rect.w != image.rect.w ||
        t->last.rect.h != image.rect.h ||
        t->last.rotate != image.rotate;

    vo_state_lock(ctx);
    if (!t->seen) {
        t->seen = 1;
        image.changed = 0;
        image.moving = 0;
    } else if (image.changed) {
        image.moving = t->changed_at && now - t->changed_at < IMAGE_SETTLE_US;
        t->changed_at = now;
        t->repaint_asked = 0;
    } else if (now - t->changed_at >= IMAGE_SETTLE_US) {
        image.moving = 0;
    }

    t->last = image;
    vo_state_unlock(ctx);

    return image;
}

static int display(Renderer *renderer, AVFrame *frame, RenderParams *params) {
    struct pl_swapchain_frame swap_frame = {0};
    struct pl_frame pl_frame = {0};
    struct pl_frame target = {0};
    RendererContext *ctx = (RendererContext *)renderer;
    ImageState image;
    ImageTracker tracked;
    struct pl_color_adjustment color_adjustment = equalizer_adjustment(params);
    struct pl_render_params pl_params;
    int ret = 0;
    bool frame_started = false;
    bool mapped_image = false;
    struct pl_color_space hint = {0};
    int64_t _ts0, _ts1, _ts2, _ts3 = 0, prs_us = 0;
    int64_t _tsc, cnv_us = 0;
    uint32_t max_dim;
    const struct pl_frame *mix_refs[LACHESIS_MAX_MIX_FRAMES];
    struct pl_frame mix_images[LACHESIS_MAX_MIX_FRAMES];
    float mix_ts[LACHESIS_MAX_MIX_FRAMES];
    uint64_t mix_sigs[LACHESIS_MAX_MIX_FRAMES];
    int num_mix;
    struct pl_frame pl_prev = {0}, pl_next = {0};
    bool mapped_prev = false, mapped_next = false;
    bool deint = params->deinterlace != 0;
    AVFrame *prev_ref = deint ? params->prev_frame : NULL;
    AVFrame *next_ref = deint ? params->next_frame : NULL;

#if LACHESIS_HAVE_D3D11
    ctx->d3d11_serial++;
#endif

    vo_state_lock(ctx);
    tracked = ctx->image;
    vo_state_unlock(ctx);
    image = track_image(ctx, params);
    pl_params = base_render_params(ctx, params, &image, &color_adjustment);

    ret = swapchain_sync_size(ctx);
    if (ret < 0) {
        goto done;
    }

    _tsc = av_gettime_relative();
    ret = convert_frame(renderer, frame);
    if (ret < 0) {
        goto done;
    }

    if (prev_ref && convert_frame(renderer, prev_ref) < 0) {
        prev_ref = NULL;
    }
    if (next_ref && convert_frame(renderer, next_ref) < 0) {
        next_ref = NULL;
    }
    cnv_us = av_gettime_relative() - _tsc;

    if (frame->width <= 0 || frame->height <= 0) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    max_dim = ctx->gpu->limits.max_tex_2d_dim;
    if (max_dim && ((unsigned)frame->width > max_dim || (unsigned)frame->height > max_dim)) {
        ret = AVERROR(ERANGE);
        goto done;
    }

    num_mix = image.changed ? 0
                            : map_frame_mix(ctx, frame, params, mix_images,
                                            mix_refs, mix_ts, mix_sigs);
    if (num_mix > 0) {
        pl_frame = mix_images[0];
    } else {
        release_mix_slots(ctx);
        if (!map_video_frame(ctx, frame, &pl_frame)) {
            ret = AVERROR_EXTERNAL;
            goto done;
        }
        mapped_image = true;
    }

    if (ctx->benchmark) {
        pl_frame.film_grain.type = PL_FILM_GRAIN_NONE;
        for (int i = 1; i < num_mix; i++) {
            mix_images[i].film_grain.type = PL_FILM_GRAIN_NONE;
        }
    }

    pl_color_space_from_avframe(&hint, frame);
    if (!ctx->have_hint || !pl_color_space_equal(&hint, &ctx->last_hint)) {
        pl_swapchain_colorspace_hint(ctx->swapchain, &hint);
        ctx->last_hint = hint;
        ctx->have_hint = true;
    }

    static int64_t t_acq, t_rnd, t_prs, t_n;

    _ts0 = av_gettime_relative();
    if (!pl_swapchain_start_frame(ctx->swapchain, &swap_frame)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    frame_started = true;
    _ts1 = av_gettime_relative();
    t_acq += _ts1 - _ts0;

    pl_frame_from_swapchain(&target, &swap_frame);

    if (ctx->have_display_hdr) {
        pl_hdr_metadata_merge(&target.color.hdr, &ctx->display_hdr);
    }

    icc_track_luma(ctx, target.color.hdr.max_luma);
    target.icc = ctx->icc_obj;
    if (ctx->cal_lut.data) {
        target.lut = &ctx->cal_lut;
        target.lut_type = PL_LUT_NORMALIZED;
    }

    struct pl_overlay overlays[LACHESIS_MAX_OVERLAYS];
    struct pl_overlay_part parts[LACHESIS_MAX_OVERLAYS];
    const struct pl_hook *hooks[LACHESIS_MAX_HOOKS];

    setup_render(ctx, &pl_frame, &target, &pl_params, params, overlays, parts,
                 hooks, &image);
    deinterlace_apply(&pl_frame, &pl_params, frame, params);

    if (pl_params.deinterlace_params &&
        pl_deinterlace_needs_refs(pl_params.deinterlace_params->algo)) {
        pl_frame.prev = map_deint_ref(ctx, ctx->prev_tex, &pl_prev, &pl_frame,
                                      prev_ref, frame);
        pl_frame.next = map_deint_ref(ctx, ctx->next_tex, &pl_next, &pl_frame,
                                      next_ref, frame);
        mapped_prev = pl_frame.prev != NULL;
        mapped_next = pl_frame.next != NULL;
    }

    _ts2 = av_gettime_relative();
    cnv_us += _ts2 - _ts1;
    if (num_mix > 0) {
        struct pl_frame_mix mix = {
            .num_frames = num_mix,
            .frames = mix_refs,
            .signatures = mix_sigs,
            .timestamps = mix_ts,
            .vsync_duration = params->mix_vsync_duration,
        };

        mix_images[0] = pl_frame;
        for (int i = 1; i < num_mix; i++) {
            mix_images[i].crop = pl_frame.crop;
            mix_images[i].rotation = pl_frame.rotation;
            mix_images[i].repr.alpha = pl_frame.repr.alpha;
        }
        pl_params.frame_mixer = &pl_filter_oversample;

        if (!pl_render_image_mix(ctx->renderer, &mix, &target, &pl_params)) {
            static bool warned_mix;
            if (!warned_mix) {
                warned_mix = true;
                log_warn("pl_render_image_mix failed! Skipping the frame.\n");
            }
            ret = AVERROR_EXTERNAL;
            goto out;
        }
    } else if (!pl_render_image(ctx->renderer, &pl_frame, &target, &pl_params)) {
        static bool warned;
        if (!warned) {
            warned = true;
            log_warn("pl_render_image failed! Skipping the frame.\n");
        }
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    _ts3 = av_gettime_relative();
    t_rnd += _ts3 - _ts2;

out:
    /* A swapchain frame that was started must always be submitted regardless of rendering failure, otherwise its
     * acquired image is never released and the next pl_swapchain_start_frame() blocks forever in AcquireNextImage. */
    if (frame_started) {
        if (!pl_swapchain_submit_frame(ctx->swapchain)) {
            if (ret == 0) {
                ret = AVERROR_EXTERNAL;
            }
        } else {
            pl_swapchain_swap_buffers(ctx->swapchain);
            if (ret == 0) {
                int64_t done = av_gettime_relative();
                prs_us = done - _ts3;
                t_prs += prs_us;
                params->present_done_us = done;
                params->present_block_us = (_ts1 - _ts0) + prs_us;
                collect_present_timing(ctx, params);
            }
        }
    }

    if (ret == 0) {
        double acq_ms = (double)(_ts1 - _ts0) / 1000.0;
        double cnv_ms = (double)cnv_us / 1000.0;
        double rnd_ms = (double)(_ts3 - _ts2) / 1000.0;
        double prs_ms = (double)prs_us / 1000.0;
        vo_state_lock(ctx);
        if (!ctx->stat_valid) {
            ctx->stat_acquire_ms = acq_ms;
            ctx->stat_convert_ms = cnv_ms;
            ctx->stat_render_ms = rnd_ms;
            ctx->stat_present_ms = prs_ms;
            ctx->stat_valid = 1;
        } else {
            const double ema_alpha = 1.0 / LACHESIS_STAT_EMA_FRAMES;
            ctx->stat_acquire_ms += (acq_ms - ctx->stat_acquire_ms) * ema_alpha;
            ctx->stat_convert_ms += (cnv_ms - ctx->stat_convert_ms) * ema_alpha;
            ctx->stat_render_ms += (rnd_ms - ctx->stat_render_ms) * ema_alpha;
            ctx->stat_present_ms += (prs_ms - ctx->stat_present_ms) * ema_alpha;
        }
        vo_state_unlock(ctx);
    }

    if (ctx->benchmark && ret == 0) {
        if (++t_n >= 120) {
            printf("acquire=%.2fms render=%.2fms present=%.2fms\n", t_acq / 1000.0 / t_n, t_rnd / 1000.0 / t_n, t_prs / 1000.0 / t_n);
            t_acq = t_rnd = t_prs = t_n = 0;
        }
    }

    if (mapped_prev) {
        pl_unmap_avframe(ctx->gpu, &pl_prev);
    }
    if (mapped_next) {
        pl_unmap_avframe(ctx->gpu, &pl_next);
    }
    if (mapped_image) {
        pl_unmap_avframe(ctx->gpu, &pl_frame);
    } else {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->mix_slots); i++) {
            if (!ctx->mix_slots[i].used) {
                unmap_mix_slot(ctx, &ctx->mix_slots[i]);
            }
        }
    }

done:
    if (ret < 0) {
        vo_state_lock(ctx);
        ctx->image = tracked;
        ctx->image.repaint_asked = 0;
        ctx->image.repaint_failed_at = av_gettime_relative();
        vo_state_unlock(ctx);
    }

    return ret;
}

static int capture(Renderer *renderer, AVFrame *frame, RenderParams *params,
                   int width, int height, uint8_t *out, int out_stride) {
    RendererContext *ctx = (RendererContext *)renderer;
    struct pl_frame pl_frame = {0};
    struct pl_frame target = {0};
    ImageState image = {.rect = params->target_rect, .rotate = params->rotate};
    struct pl_color_adjustment color_adjustment = equalizer_adjustment(params);
    struct pl_render_params pl_params;
    pl_tex cap_tex = NULL;
    struct pl_tex_params cap_params;
    struct pl_tex_transfer_params xfer;
    struct pl_frame pl_prev = {0}, pl_next = {0};
    bool mapped_prev = false, mapped_next = false;
    int ret = 0;
    bool deint = params->deinterlace != 0;
    AVFrame *prev_ref = deint ? params->prev_frame : NULL;
    AVFrame *next_ref = deint ? params->next_frame : NULL;

#if LACHESIS_HAVE_D3D11
    ctx->d3d11_serial++;
#endif

    pl_params = base_render_params(ctx, params, &image, &color_adjustment);

    ret = convert_frame(renderer, frame);
    if (ret < 0) {
        return ret;
    }

    if (prev_ref && convert_frame(renderer, prev_ref) < 0) {
        prev_ref = NULL;
    }
    if (next_ref && convert_frame(renderer, next_ref) < 0) {
        next_ref = NULL;
    }

    if (!map_video_frame(ctx, frame, &pl_frame)) {
        return AVERROR_EXTERNAL;
    }

    pl_fmt fmt = pl_find_named_fmt(ctx->gpu, "rgba8");
    if (!fmt) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    if (!(fmt->caps & PL_FMT_CAP_HOST_READABLE)) {
        ret = AVERROR(ENOSYS);
        goto out;
    }
    cap_params = (struct pl_tex_params){
        .w = width,
        .h = height,
        .format = fmt,
        .renderable = true,
        .host_readable = true,
        .blit_dst = true,
    };
    cap_tex = pl_tex_create(ctx->gpu, &cap_params);
    if (!cap_tex) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

    target.num_planes = 1;
    target.planes[0] = (struct pl_plane){
        .texture = cap_tex,
        .components = 4,
        .component_mapping = {0, 1, 2, 3},
    };
    target.repr = (struct pl_color_repr){
        .sys = PL_COLOR_SYSTEM_RGB,
        .levels = PL_COLOR_LEVELS_FULL,
        .alpha = PL_ALPHA_INDEPENDENT,
    };
    target.color = pl_color_space_srgb;

    struct pl_overlay overlays[LACHESIS_MAX_OVERLAYS];
    struct pl_overlay_part parts[LACHESIS_MAX_OVERLAYS];
    const struct pl_hook *hooks[LACHESIS_MAX_HOOKS];

    setup_render(ctx, &pl_frame, &target, &pl_params, params, overlays, parts,
                 hooks, &image);
    deinterlace_apply(&pl_frame, &pl_params, frame, params);

    if (pl_params.deinterlace_params &&
        pl_deinterlace_needs_refs(pl_params.deinterlace_params->algo)) {
        pl_frame.prev = map_deint_ref(ctx, ctx->prev_tex, &pl_prev, &pl_frame,
                                      prev_ref, frame);
        pl_frame.next = map_deint_ref(ctx, ctx->next_tex, &pl_next, &pl_frame,
                                      next_ref, frame);
        mapped_prev = pl_frame.prev != NULL;
        mapped_next = pl_frame.next != NULL;
    }

    if (!pl_render_image(ctx->renderer, &pl_frame, &target, &pl_params)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

    xfer = (struct pl_tex_transfer_params){
        .tex = cap_tex,
        .ptr = out,
        .row_pitch = out_stride,
    };
    if (!pl_tex_download(ctx->gpu, &xfer)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

out:
    if (cap_tex) {
        pl_tex_destroy(ctx->gpu, &cap_tex);
    }
    if (mapped_prev) {
        pl_unmap_avframe(ctx->gpu, &pl_prev);
    }
    if (mapped_next) {
        pl_unmap_avframe(ctx->gpu, &pl_next);
    }
    pl_unmap_avframe(ctx->gpu, &pl_frame);
    return ret;
}

static int resize(Renderer *renderer, int width, int height) {
    RendererContext *ctx = (RendererContext *)renderer;
    int w = width, h = height;

    if (!ctx || !ctx->swapchain) {
        return AVERROR(EINVAL);
    }
    if (!pl_swapchain_resize(ctx->swapchain, &w, &h)) {
        vo_state_lock(ctx);
        ctx->swapchain_stale = 1;
        ctx->swapchain_stale_w = width;
        ctx->swapchain_stale_h = height;
        ctx->swapchain_retry = 0;
        vo_state_unlock(ctx);
        return AVERROR_EXTERNAL;
    }
    vo_state_lock(ctx);
    ctx->swapchain_stale = 0;
    ctx->swapchain_retry = 0;
    vo_state_unlock(ctx);
    ctx->present_timing_silent = 0;

    return 0;
}

#define LACHESIS_SELF_TEST_SIZE 64

static AVFrame *alloc_self_test_frame(int value) {
    AVFrame *frame = av_frame_alloc();

    if (!frame) {
        return NULL;
    }
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = LACHESIS_SELF_TEST_SIZE;
    frame->height = LACHESIS_SELF_TEST_SIZE;
    frame->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return NULL;
    }
    for (int y = 0; y < frame->height; y++) {
        uint8_t *row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < frame->width * 4; x += 4) {
            row[x + 0] = value;
            row[x + 1] = value;
            row[x + 2] = value;
            row[x + 3] = 255;
        }
    }

    return frame;
}

static void vo_pin_gpu(RendererContext *ctx) {
#if LACHESIS_HAVE_OPENGL
    gl_pin_current(ctx);
#else
    (void)ctx;
#endif
}

static void vo_unpin_gpu(RendererContext *ctx) {
#if LACHESIS_HAVE_OPENGL
    gl_unpin_current(ctx);
#else
    (void)ctx;
#endif
}

static int display_blank(Renderer *renderer, RenderParams *params) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx->blank_frame) {
        ctx->blank_frame = alloc_self_test_frame(0);
        if (!ctx->blank_frame) {
            return AVERROR(ENOMEM);
        }
    }

    return display(renderer, ctx->blank_frame, params);
}

static int self_test(Renderer *renderer, int width, int height) {
    enum { size = LACHESIS_SELF_TEST_SIZE };
    RenderParams params = {.target_rect = {0, 0, size, size}};
    AVFrame *frame;
    uint8_t *pixels;
    int bright = 0;
    int ret;

    frame = alloc_self_test_frame(255);
    pixels = frame ? av_mallocz(size * size * 4) : NULL;
    if (!pixels) {
        /* Being out of memory is not the renderer's fault. */
        av_frame_free(&frame);
        return 0;
    }

    ret = capture(renderer, frame, &params, size, size, pixels, size * 4);
    if (ret == 0) {
        for (int i = 0; i < size * size; i++) {
            const uint8_t *px = &pixels[i * 4];
            bright += px[0] + px[1] + px[2] >= 3 * 128;
        }
        if (bright < size * size / 2) {
            ret = AVERROR_EXTERNAL;
        }
    } else if (ret == AVERROR(ENOSYS)) {
        ret = 0;
    }
    av_free(pixels);
    av_frame_free(&frame);
    if (ret < 0) {
        return ret;
    }

    frame = alloc_self_test_frame(0);
    if (!frame) {
        return 0;
    }
    params.target_rect.w = width > 0 ? width : size;
    params.target_rect.h = height > 0 ? height : size;
    for (int attempt = 0;; attempt++) {
        ret = display(renderer, frame, &params);
        if (ret == 0 || attempt >= 2) {
            break;
        }
        av_usleep(50000);
    }
    av_frame_free(&frame);

    return ret;
}

static void destroy(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    av_frame_free(&ctx->blank_frame);
    av_frame_free(&ctx->sw_frame);
    hwdownload_free(&ctx->readback);
    av_freep(&ctx->pixfmts);
    ctx->num_pixfmts = 0;

    if (ctx->sbs360_hook) {
        view360_pl_hook_destroy(&ctx->sbs360_hook);
    }
    if (ctx->supersample_hook) {
        supersample_pl_hook_destroy(&ctx->supersample_hook);
    }

    icc_forget(ctx);
    cal_drop(ctx);

    if (ctx->gpu) {
        if (!ctx->quiesced || ctx->gpu_busy) {
            gpu_quiesce(ctx);
        }
        shader_cache_close(&ctx->shader_cache, ctx->gpu);
        pl_tex_destroy(ctx->gpu, &ctx->osd_tex);
        pl_tex_destroy(ctx->gpu, &ctx->sub_tex);
        pl_tex_destroy(ctx->gpu, &ctx->text_sub_tex);
        for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->prev_tex); i++) {
            pl_tex_destroy(ctx->gpu, &ctx->prev_tex[i]);
            pl_tex_destroy(ctx->gpu, &ctx->next_tex[i]);
        }
        for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->tex); i++) {
            pl_tex_destroy(ctx->gpu, &ctx->tex[i]);
        }
        destroy_mix_slots(ctx);
        pl_renderer_destroy(&ctx->renderer);
    }

    switch (renderer->backend) {
#if LACHESIS_HAVE_VULKAN
    case RENDERER_API_VULKAN:
        vk_backend_destroy(ctx);
        break;
#endif
#if LACHESIS_HAVE_OPENGL
    case RENDERER_API_OPENGL:
        gl_backend_destroy(ctx);
        break;
#endif
#if LACHESIS_HAVE_D3D11
    case RENDERER_API_D3D11:
        d3d11_backend_destroy(ctx);
        break;
#endif
    default:
        break;
    }
    ctx->gpu = NULL;

    pl_log_destroy(&ctx->log_ctx);
}

static int enable_360(RendererContext *ctx, enum View360Layout layout,
                      enum View360Projection projection);

#define VO_HANDOFF_WAIT_MS 8
#define VO_WINDOW_WAIT_MS 60
#define VO_BORROW_WAIT_MS 250
#define VO_DRAIN_WAIT_MS 1000
#define VO_CAPTURE_WAIT_MS 1000
#define VO_STOP_WAIT_MS 500

static void vo_overlay_free(VoOverlay *ov) {
    av_freep(&ov->pixels);
    ov->size = 0;
    ov->generation = 0;
}

static void *vo_overlay_take(VoOverlay *ov, const void *pixels, int height,
                             int stride, unsigned generation) {
    size_t need;

    if (!pixels || height <= 0 || stride <= 0) {
        return NULL;
    }
    need = (size_t)height * (size_t)stride;
    if (ov->pixels && ov->size == need && ov->generation == generation) {
        return ov->pixels;
    }
    if (!ov->pixels || ov->size != need) {
        av_freep(&ov->pixels);
        if (!(ov->pixels = av_malloc(need))) {
            ov->size = 0;
            ov->generation = 0;
            return NULL;
        }
        ov->size = need;
    }
    memcpy(ov->pixels, pixels, need);
    ov->generation = generation;

    return ov->pixels;
}

static int vo_frame_take(VoFrame *dst, AVFrame *src, uint64_t id) {
    if (!src) {
        if (dst->frame) {
            av_frame_unref(dst->frame);
        }
        dst->id = 0;
        return 0;
    }
    if (dst->frame && dst->frame->buf[0] && id && dst->id == id) {
        return 0;
    }
    if (!dst->frame && !(dst->frame = av_frame_alloc())) {
        return AVERROR(ENOMEM);
    }
    av_frame_unref(dst->frame);
    dst->id = 0;
    if (av_frame_ref(dst->frame, src) < 0) {
        return AVERROR(ENOMEM);
    }
    dst->id = id;

    return 0;
}

static void vo_frame_drop(VoFrame *vf) {
    if (vf->frame) {
        av_frame_unref(vf->frame);
    }
    vf->id = 0;
}

static void vo_frame_free(VoFrame *vf) {
    av_frame_free(&vf->frame);
    vf->id = 0;
}

static void vo_job_release(Vo *vo) {
    vo->params.mix_frames = NULL;
    vo->params.mix_num_frames = 0;
    vo->params.prev_frame = NULL;
    vo->params.next_frame = NULL;
}

static void vo_frames_drop(Vo *vo) {
    vo_frame_drop(&vo->frame);
    vo_frame_drop(&vo->prev_frame);
    vo_frame_drop(&vo->next_frame);
    for (size_t i = 0; i < FF_ARRAY_ELEMS(vo->mix_frame); i++) {
        vo_frame_drop(&vo->mix_frame[i]);
    }
}

/* Called with the lock held. */
static int vo_job_take(Vo *vo, AVFrame *frame, const RenderParams *params,
                       int blank) {
    int num_mix = params->mix_frames ? params->mix_num_frames : 0;

    vo->params = *params;
    vo->blank = blank;

    if (num_mix > (int)FF_ARRAY_ELEMS(vo->mix_frame)) {
        num_mix = FF_ARRAY_ELEMS(vo->mix_frame);
    }
    if (vo_frame_take(&vo->frame, blank ? NULL : frame, params->frame_id) < 0 ||
        vo_frame_take(&vo->prev_frame, params->prev_frame,
                      params->prev_frame_id) < 0 ||
        vo_frame_take(&vo->next_frame, params->next_frame,
                      params->next_frame_id) < 0) {
        vo_frames_drop(vo);
        return AVERROR(ENOMEM);
    }
    vo->params.prev_frame = params->prev_frame ? vo->prev_frame.frame : NULL;
    vo->params.next_frame = params->next_frame ? vo->next_frame.frame : NULL;

    for (int i = 0; i < num_mix; i++) {
        AVFrame *src = params->mix_frames[i].frame;

        vo->mix[i] = params->mix_frames[i];
        if (src && src == frame) {
            vo->mix[i].frame = vo->frame.frame;
            continue;
        }
        if (vo_frame_take(&vo->mix_frame[i], src,
                          params->mix_frames[i].signature) < 0) {
            vo_frames_drop(vo);
            return AVERROR(ENOMEM);
        }
        vo->mix[i].frame = src ? vo->mix_frame[i].frame : NULL;
    }
    vo->params.mix_frames = num_mix ? vo->mix : NULL;
    vo->params.mix_num_frames = num_mix;

    vo->params.osd_pixels =
        vo_overlay_take(&vo->osd, params->osd_pixels, params->osd_height,
                        params->osd_stride, params->osd_generation);
    vo->params.sub_pixels =
        vo_overlay_take(&vo->sub, params->sub_pixels, params->sub_height,
                        params->sub_stride, params->sub_generation);
    vo->params.text_sub_pixels =
        vo_overlay_take(&vo->text_sub, params->text_sub_pixels,
                        params->text_sub_height, params->text_sub_stride,
                        params->text_sub_generation);

    return 0;
}

static void vo_note_feedback(Vo *vo, const RenderParams *rp, unsigned epoch) {
    unsigned next = (vo->feedback_head + 1) % VO_FEEDBACK_RING;
    VoFeedback *fb;

    if (rp->present_done_us <= 0 && rp->present_display_us <= 0) {
        return;
    }
    if (epoch != vo->feedback_epoch) {
        return;
    }
    if (next == vo->feedback_tail) {
        vo->feedback_tail = (vo->feedback_tail + 1) % VO_FEEDBACK_RING;
    }
    fb = &vo->feedback[vo->feedback_head];
    fb->done_us = rp->present_done_us;
    fb->block_us = rp->present_block_us;
    fb->source = rp->present_source;
    fb->display_us = rp->present_display_us;
    fb->refresh_us = rp->present_refresh_us;
    vo->feedback_head = next;
}

static int vo_thread(void *arg) {
    RendererContext *ctx = arg;
    Vo *vo = &ctx->vo;

    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    for (;;) {
        enum View360Layout p360_layout;
        enum View360Projection p360_projection;
        enum SupersampleLevel p_supersample;
        AVFrame *frame;
        float yaw, pitch, roll, hfov;
        unsigned epoch;
        unsigned pending;
        int blank;
        int status;

        SDL_LockMutex(vo->lock);
        while (!vo->have_job && !vo->quit) {
            SDL_WaitCondition(vo->wake, vo->lock);
        }
        if (vo->quit && !vo->have_job) {
            SDL_UnlockMutex(vo->lock);
            break;
        }
        vo->have_job = 0;
        blank = vo->blank;
        frame = vo->frame.frame;
        pending = vo->pending;
        vo->pending = 0;
        p360_layout = vo->pending_360_layout;
        p360_projection = vo->pending_360_projection;
        p_supersample = vo->pending_supersample;
        epoch = vo->feedback_epoch;
        yaw = vo->view360_yaw;
        pitch = vo->view360_pitch;
        roll = vo->view360_roll;
        hfov = vo->view360_hfov;
        SDL_UnlockMutex(vo->lock);

        if (pending & VO_PENDING_360) {
            enable_360(ctx, p360_layout, p360_projection);
        }
        ctx->sbs360_yaw = yaw;
        ctx->sbs360_pitch = pitch;
        ctx->sbs360_roll = roll;
        ctx->sbs360_hfov = hfov;
        if (pending & VO_PENDING_SUPERSAMPLE) {
            if (p_supersample != SUPERSAMPLE_OFF && !ctx->supersample_hook) {
                ctx->supersample_hook = supersample_pl_hook_create(ctx->gpu);
                if (!ctx->supersample_hook) {
                    log_warn("Supersampling is unavailable.\n");
                }
            }
            ctx->supersample_level =
                ctx->supersample_hook ? p_supersample : SUPERSAMPLE_OFF;
        }

        vo_pin_gpu(ctx);
        vo_pin_gpu(ctx);
        status = blank ? display_blank(&ctx->api, &vo->params)
                       : display(&ctx->api, frame, &vo->params);
        vo_unpin_gpu(ctx);
        vo_unpin_gpu(ctx);

        SDL_LockMutex(vo->lock);
        vo->last_status = status;
        vo->have_status = 1;
        vo_note_feedback(vo, &vo->params, epoch);
        vo_job_release(vo);
        vo->busy = 0;
        SDL_BroadcastCondition(vo->idle);
        SDL_UnlockMutex(vo->lock);
    }

    return 0;
}

static int vo_start(RendererContext *ctx) {
    Vo *vo = &ctx->vo;

    if (ctx->api.backend == RENDERER_API_OPENGL) {
        return 0;
    }
    vo->view360_hfov = 90.0f;
    if (!(vo->lock = SDL_CreateMutex()) || !(vo->wake = SDL_CreateCondition()) ||
        !(vo->idle = SDL_CreateCondition())) {
        return AVERROR(ENOMEM);
    }
    if (!(vo->thread = SDL_CreateThread(vo_thread, "video output", ctx))) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int vo_stop(RendererContext *ctx) {
    Vo *vo = &ctx->vo;
    int stopped = 1;

    if (vo->abandoned) {
        return 0;
    }
    if (vo->thread) {
        SDL_LockMutex(vo->lock);
        vo->quit = 1;
        SDL_SignalCondition(vo->wake);
        while (vo->busy) {
            if (!SDL_WaitConditionTimeout(vo->idle, vo->lock, VO_STOP_WAIT_MS)) {
                break;
            }
        }
        stopped = !vo->busy;
        SDL_UnlockMutex(vo->lock);

        if (stopped) {
            SDL_WaitThread(vo->thread, NULL);
        } else {
            SDL_DetachThread(vo->thread);
            vo->abandoned = 1;
        }
        vo->thread = NULL;
    }
    if (!stopped) {
        return 0;
    }

    vo_job_release(vo);
    vo_frame_free(&vo->frame);
    vo_frame_free(&vo->prev_frame);
    vo_frame_free(&vo->next_frame);
    for (size_t i = 0; i < FF_ARRAY_ELEMS(vo->mix_frame); i++) {
        vo_frame_free(&vo->mix_frame[i]);
    }
    vo_overlay_free(&vo->osd);
    vo_overlay_free(&vo->sub);
    vo_overlay_free(&vo->text_sub);
    if (vo->wake) {
        SDL_DestroyCondition(vo->wake);
        vo->wake = NULL;
    }
    if (vo->idle) {
        SDL_DestroyCondition(vo->idle);
        vo->idle = NULL;
    }
    if (vo->lock) {
        SDL_DestroyMutex(vo->lock);
        vo->lock = NULL;
    }

    return 1;
}

static int vo_borrow(RendererContext *ctx, int timeout_ms) {
    Vo *vo = &ctx->vo;

    if (ctx->quiesced) {
        return 0;
    }

    if (!vo->thread) {
        return !vo->abandoned;
    }
    SDL_LockMutex(vo->lock);
    if (vo->borrowed) {
        vo->borrowed++;
        SDL_UnlockMutex(vo->lock);
        return 1;
    }
    while (vo->busy) {
        if (!SDL_WaitConditionTimeout(vo->idle, vo->lock, timeout_ms)) {
            break;
        }
    }
    if (vo->busy) {
        SDL_UnlockMutex(vo->lock);
        return 0;
    }
    vo->busy = 1;
    vo->borrowed = 1;
    SDL_UnlockMutex(vo->lock);

    return 1;
}

static void vo_release(RendererContext *ctx) {
    Vo *vo = &ctx->vo;

    if (!vo->thread) {
        return;
    }
    SDL_LockMutex(vo->lock);
    if (vo->borrowed && !--vo->borrowed) {
        vo->busy = 0;
        SDL_BroadcastCondition(vo->idle);
    }
    SDL_UnlockMutex(vo->lock);
}

static int vo_submit(RendererContext *ctx, AVFrame *frame, RenderParams *params,
                     int blank) {
    Vo *vo = &ctx->vo;
    int status;
    int ret;

    if (ctx->quiesced) {
        return AVERROR(EAGAIN);
    }

    if (!vo->thread) {
        if (vo->abandoned) {
            return AVERROR(EAGAIN);
        }
        vo_pin_gpu(ctx);
        status = blank ? display_blank(&ctx->api, params)
                       : display(&ctx->api, frame, params);
        vo_unpin_gpu(ctx);
        vo_note_feedback(vo, params, vo->feedback_epoch);

        return status;
    }

    SDL_LockMutex(vo->lock);
    if (vo->busy) {
        int64_t deadline = av_gettime_relative() + VO_HANDOFF_WAIT_MS * 1000;

        while (vo->busy) {
            int64_t left = ctx->benchmark ? 1000 : deadline - av_gettime_relative();

            if (left <= 0) {
                break;
            }
            SDL_WaitConditionTimeout(vo->idle, vo->lock,
                                     (Sint32)(left / 1000) + 1);
        }
    }
    if (vo->busy || vo->quit) {
        SDL_UnlockMutex(vo->lock);
        return AVERROR(EAGAIN);
    }
    ret = vo_job_take(vo, frame, params, blank);
    if (ret < 0) {
        SDL_UnlockMutex(vo->lock);
        return ret;
    }
    status = vo->have_status ? vo->last_status : AVERROR(ERANGE);
    vo->have_job = 1;
    vo->busy = 1;
    SDL_SignalCondition(vo->wake);
    SDL_UnlockMutex(vo->lock);

    return status;
}

void renderer_drop_present_feedback(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return;
    }
    vo_state_lock(ctx);
    ctx->vo.feedback_tail = ctx->vo.feedback_head;
    ctx->vo.feedback_epoch++;
    vo_state_unlock(ctx);
}

int renderer_pause_output(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return 1;
    }

    return vo_borrow(ctx, VO_WINDOW_WAIT_MS);
}

void renderer_resume_output(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (ctx) {
        vo_release(ctx);
    }
}

int renderer_take_present_feedback(Renderer *renderer,
                                   RendererPresentFeedback *out) {
    RendererContext *ctx = (RendererContext *)renderer;
    Vo *vo;
    VoFeedback fb;

    if (!ctx) {
        return 0;
    }
    vo = &ctx->vo;
    vo_state_lock(ctx);
    if (vo->feedback_tail == vo->feedback_head) {
        vo_state_unlock(ctx);
        return 0;
    }
    fb = vo->feedback[vo->feedback_tail];
    vo->feedback_tail = (vo->feedback_tail + 1) % VO_FEEDBACK_RING;
    vo_state_unlock(ctx);

    out->done_us = fb.done_us;
    out->block_us = fb.block_us;
    out->source = fb.source;
    out->display_us = fb.display_us;
    out->refresh_us = fb.refresh_us;

    return 1;
}

static const AVClass renderer_class = {
    .class_name = "Renderer",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

static const enum RendererApi renderer_api_order[] = {
#if LACHESIS_HAVE_D3D11
    RENDERER_API_D3D11,
#endif
#if LACHESIS_HAVE_VULKAN
    RENDERER_API_VULKAN,
#endif
#if LACHESIS_HAVE_OPENGL
    RENDERER_API_OPENGL,
#endif
};

static Renderer *renderer_alloc(enum RendererApi api) {
    RendererContext *ctx = av_mallocz(sizeof(*ctx));

    if (!ctx) {
        return NULL;
    }
    ctx->api.class = &renderer_class;
    ctx->api.backend = api;

    return &ctx->api;
}

static Uint32 api_window_flag(enum RendererApi api) {
    switch (api) {
    case RENDERER_API_VULKAN:
        return SDL_WINDOW_VULKAN;
    case RENDERER_API_OPENGL:
        return SDL_WINDOW_OPENGL;
    default:
        return 0;
    }
}

static const char *api_label(enum RendererApi api) {
    switch (api) {
    case RENDERER_API_VULKAN:
        return "Vulkan";
    case RENDERER_API_OPENGL:
        return "OpenGL";
    case RENDERER_API_D3D11:
        return "Direct3D 11";
    default:
        return "unknown";
    }
}

static int api_num_attempts(enum RendererApi api, const AVDictionary *opt) {
#if LACHESIS_HAVE_OPENGL
    if (api == RENDERER_API_OPENGL) {
        return gl_num_attempts(opt);
    }
#endif
    (void)api;
    (void)opt;
    return 1;
}

static const char *api_prepare_attempt(enum RendererApi api, int attempt,
                                       const AVDictionary *opt) {
#if LACHESIS_HAVE_OPENGL
    if (api == RENDERER_API_OPENGL) {
        return gl_apply_profile_hints(attempt, opt);
    }
#endif
    (void)attempt;
    (void)opt;

    return api_label(api);
}

static void note_failure(char *why, size_t why_size, const char *what,
                         const char *detail, int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    char line[256];
    size_t len = strlen(why);

    if (!detail || !*detail) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        detail = errbuf;
    }
    log_verbose("Failed to open the %s renderer: %s.\n", what, detail);
    if (len && strstr(why, detail)) {
        return;
    }
    snprintf(line, sizeof(line), "%s%s: %s", len ? "; " : "", what, detail);
    av_strlcat(why, line, why_size);
}

static int renderer_try(const RendererOpenParams *params, enum RendererApi api,
                        int attempt, SDL_Window **out_window,
                        Renderer **out_renderer, char *why, size_t why_size) {
    SDL_Window *window;
    Renderer *renderer;
    const char *what;
    const char *video_driver;
    int show_before_test;
    int w = 0, h = 0;
    int ret;

    what = api_prepare_attempt(api, attempt, params->opt);

    SDL_ClearError();

    window = SDL_CreateWindow(params->title, params->width, params->height,
                              params->window_flags | api_window_flag(api));
    if (!window) {
        note_failure(why, why_size, what, SDL_GetError(), AVERROR_EXTERNAL);
        return AVERROR_EXTERNAL;
    }
    icon_set_window_icon(window);

    renderer = renderer_alloc(api);
    if (!renderer) {
        SDL_DestroyWindow(window);
        return AVERROR(ENOMEM);
    }
    ((RendererContext *)renderer)->window = window;

    ret = create(renderer, window, params->opt);
    if (ret < 0) {
        note_failure(why, why_size, what, SDL_GetError(), ret);
        goto fail;
    }

    video_driver = SDL_GetCurrentVideoDriver();
    show_before_test = video_driver && !strcmp(video_driver, "wayland");

    if (show_before_test) {
        SDL_ShowWindow(window);
    }

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w > 0 && h > 0) {
        resize(renderer, w, h);
    }

    ret = self_test(renderer, w, h);
    if (ret < 0) {
        note_failure(why, why_size, what, "initialized but cannot render", ret);
        goto fail;
    }

    if (!show_before_test) {
        SDL_ShowWindow(window);
    }

    ret = vo_start((RendererContext *)renderer);
    if (ret < 0) {
        note_failure(why, why_size, what, "no thread to present on", ret);
        goto fail;
    }

    *out_window = window;
    *out_renderer = renderer;

    return 0;

fail:
    if (vo_stop((RendererContext *)renderer)) {
        destroy(renderer);
        av_free(renderer);
        SDL_DestroyWindow(window);
    }

    return ret;
}

static void note_ignored_requests(Renderer *renderer) {
    if (!renderer || renderer_api(renderer) == RENDERER_API_VULKAN) {
        return;
    }
    if (renderer_want_device && renderer_api(renderer) == RENDERER_API_OPENGL) {
    }
    if (renderer_want_translucent) {
        log_warn("A translucent -video-bg needs the Vulkan renderer but on %s "
                 "the background is opaque.\n",
                 renderer_api_name(renderer));
    }
}

int renderer_open(const RendererOpenParams *params, SDL_Window **window,
                  Renderer **out, char *why, size_t why_size) {
    enum RendererApi order[FF_ARRAY_ELEMS(renderer_api_order)];
    const char *driver = SDL_GetCurrentVideoDriver();
    size_t num = 0;
    int last = AVERROR(ENOSYS);

    why[0] = '\0';
    renderer_want_translucent = params->translucent;
    renderer_want_device = params->device && params->device[0] ? params->device : NULL;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(renderer_api_order); i++) {
        enum RendererApi api = renderer_api_order[i];

        if (params->exclude & (1u << api)) {
            continue;
        }
        if (params->api != RENDERER_API_AUTO && api != params->api) {
            continue;
        }
        order[num++] = api;
    }

    if (!num) {
        snprintf(why, why_size, "no backend is compiled in or left enabled%s",
                 params->api != RENDERER_API_AUTO ? " for the requested GPU API" : "");
        return AVERROR(ENOSYS);
    }

    log_verbose("SDL video driver: %s.\n", driver ? driver : "none");

    for (int pass = 0; pass < 2; pass++) {
        int hardware_only = pass == 0 && num > 1;

        renderer_allow_software_gpu = !hardware_only;
        why[0] = '\0';

        for (size_t i = 0; i < num; i++) {
            enum RendererApi api = order[i];
            int attempts = api_num_attempts(api, params->opt);

            for (int attempt = 0; attempt < attempts; attempt++) {
                int ret = renderer_try(params, api, attempt, window, out, why,
                                       why_size);

                if (ret >= 0) {
                    note_ignored_requests(*out);
                    return 0;
                }
                last = ret;
            }
            if (i + 1 < num) {
                log_verbose("The %s renderer is unavailable. Trying %s.\n",
                            api_label(api), api_label(order[i + 1]));
            }
        }
        if (!hardware_only) {
            break;
        }
    }

    if (!why[0]) {
        snprintf(why, why_size, "no reason reported");
    }

    return last;
}

enum RendererApi renderer_api(const Renderer *renderer) {
    return renderer ? renderer->backend : RENDERER_API_AUTO;
}

const char *renderer_api_name(const Renderer *renderer) {
    const RendererContext *ctx = (const RendererContext *)renderer;

    if (!ctx || !ctx->api_name[0]) {
        return "none";
    }

    return ctx->api_name;
}

const char *renderer_device_name(const Renderer *renderer) {
    const RendererContext *ctx = (const RendererContext *)renderer;

    if (!ctx || !ctx->device_name[0]) {
        return NULL;
    }

    return ctx->device_name;
}

int renderer_refresh_display_info(Renderer *renderer, SDL_Window *window) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (!ctx) {
        return 0;
    }
    if (!vo_borrow(ctx, 0)) {
        return AVERROR(EAGAIN);
    }
    ret = icc_load_display(ctx, window) | hdr_refresh(ctx, window);
    vo_release(ctx);

    return ret;
}

static int enable_360(RendererContext *ctx, enum View360Layout layout,
                      enum View360Projection projection) {
    int enable = layout != VIEW360_LAYOUT_OFF;

    if (enable && !ctx->sbs360_hook) {
        ctx->sbs360_hook = view360_pl_hook_create(ctx->gpu);
        if (!ctx->sbs360_hook) {
            return AVERROR_EXTERNAL;
        }
        ctx->sbs360_yaw = 0.0f;
        ctx->sbs360_pitch = 0.0f;
        ctx->sbs360_roll = 0.0f;
        ctx->sbs360_hfov = 90.0f;
    } else if (!enable && ctx->sbs360_hook) {
        view360_pl_hook_destroy(&ctx->sbs360_hook);
    }
    ctx->sbs360_enabled = enable;
    ctx->sbs360_layout = layout;
    ctx->sbs360_projection = projection;

    return 0;
}

int renderer_enable_360(Renderer *renderer, enum View360Layout layout,
                        enum View360Projection projection) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (!ctx) {
        return AVERROR(EINVAL);
    }
    if (!vo_borrow(ctx, VO_BORROW_WAIT_MS)) {
        vo_state_lock(ctx);
        ctx->vo.pending_360_layout = layout;
        ctx->vo.pending_360_projection = projection;
        ctx->vo.pending |= VO_PENDING_360;
        vo_state_unlock(ctx);
        return 0;
    }
    vo_state_lock(ctx);
    ctx->vo.pending &= ~VO_PENDING_360;
    vo_state_unlock(ctx);
    ret = enable_360(ctx, layout, projection);
    vo_release(ctx);

    return ret;
}

int renderer_set_supersample(Renderer *renderer, enum SupersampleLevel level) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return AVERROR(EINVAL);
    }
    if (!vo_borrow(ctx, VO_BORROW_WAIT_MS)) {
        vo_state_lock(ctx);
        ctx->vo.pending_supersample = level;
        ctx->vo.pending |= VO_PENDING_SUPERSAMPLE;
        vo_state_unlock(ctx);
        return 0;
    }
    vo_state_lock(ctx);
    ctx->vo.pending &= ~VO_PENDING_SUPERSAMPLE;
    vo_state_unlock(ctx);
    if (level != SUPERSAMPLE_OFF && !ctx->supersample_hook) {
        ctx->supersample_hook = supersample_pl_hook_create(ctx->gpu);
        if (!ctx->supersample_hook) {
            vo_release(ctx);
            return AVERROR_EXTERNAL;
        }
    }
    ctx->supersample_level = level;
    vo_release(ctx);

    return 0;
}

void renderer_update_360(Renderer *renderer, float yaw, float pitch, float roll, float hfov) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return;
    }
    if (ctx->vo.lock) {
        SDL_LockMutex(ctx->vo.lock);
    }
    ctx->vo.view360_yaw = yaw;
    ctx->vo.view360_pitch = pitch;
    ctx->vo.view360_roll = roll;
    ctx->vo.view360_hfov = hfov;
    if (ctx->vo.lock) {
        SDL_UnlockMutex(ctx->vo.lock);
    } else {
        ctx->sbs360_yaw = yaw;
        ctx->sbs360_pitch = pitch;
        ctx->sbs360_roll = roll;
        ctx->sbs360_hfov = hfov;
    }
}

int renderer_take_image_repaint(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;
    int64_t now;
    int take = 0;

    if (!ctx) {
        return 0;
    }
    if (ctx->vo.lock) {
        SDL_LockMutex(ctx->vo.lock);
    }
    now = av_gettime_relative();
    if (ctx->image.last.moving && !ctx->image.repaint_asked &&
        now - ctx->image.changed_at >= IMAGE_SETTLE_US &&
        (!ctx->image.repaint_failed_at ||
         now - ctx->image.repaint_failed_at >= IMAGE_SETTLE_US)) {
        ctx->image.repaint_asked = 1;
        take = 1;
    }
    if (ctx->vo.lock) {
        SDL_UnlockMutex(ctx->vo.lock);
    }

    return take;
}

int renderer_device_node(Renderer *renderer, char *buf, size_t size) {
    if (!buf || !size) {
        return AVERROR(EINVAL);
    }
    buf[0] = '\0';

#ifdef LACHESIS_HAVE_DRM_NODES
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return AVERROR(ENOSYS);
    }

#ifdef LACHESIS_HAVE_VK_DRM_NODE
    if (ctx->api.backend == RENDERER_API_VULKAN && ctx->hw_device_ref) {
        const AVHWDeviceContext *dev =
            (const AVHWDeviceContext *)ctx->hw_device_ref->data;
        const AVVulkanDeviceContext *hwctx = dev->hwctx;

        if (dev->type != AV_HWDEVICE_TYPE_VULKAN) {
            return AVERROR(ENOSYS);
        }

        return vk_render_node(hwctx->get_proc_addr, hwctx->inst,
                              hwctx->phys_dev, buf, size);
    }
#endif

#if LACHESIS_HAVE_OPENGL
    if (ctx->api.backend == RENDERER_API_OPENGL) {
        if (!ctx->gl_drm_node[0]) {
            return AVERROR(ENOSYS);
        }
        av_strlcpy(buf, ctx->gl_drm_node, size);

        return 0;
    }
#endif
#else
    (void)renderer;
#endif

    return AVERROR(ENOSYS);
}

const char *renderer_wanted_device(void) {
    return renderer_want_device;
}

int renderer_get_hw_dev(Renderer *renderer, AVBufferRef **dev) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (renderer && ctx->hw_device_ref) {
        *dev = ctx->hw_device_ref;
        return 0;
    }
    *dev = NULL;

    return AVERROR(ENOSYS);
}

int renderer_display(Renderer *renderer, AVFrame *frame, RenderParams *render_params) {
    return vo_submit((RendererContext *)renderer, frame, render_params, 0);
}

int renderer_display_blank(Renderer *renderer, RenderParams *render_params) {
    return vo_submit((RendererContext *)renderer, NULL, render_params, 1);
}

int renderer_capture(Renderer *renderer, AVFrame *frame, RenderParams *render_params,
                     int width, int height, uint8_t *out, int out_stride) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (!vo_borrow(ctx, VO_CAPTURE_WAIT_MS)) {
        return AVERROR(EAGAIN);
    }
    ret = capture(renderer, frame, render_params, width, height, out, out_stride);
    vo_release(ctx);

    return ret;
}

int renderer_resize(Renderer *renderer, int width, int height) {
    RendererContext *ctx = (RendererContext *)renderer;
    int ret;

    if (!ctx) {
        return AVERROR(EINVAL);
    }
    if (!vo_borrow(ctx, VO_BORROW_WAIT_MS)) {
        vo_state_lock(ctx);
        if (!ctx->swapchain_stale || ctx->swapchain_stale_w != width ||
            ctx->swapchain_stale_h != height) {
            ctx->swapchain_stale = 1;
            ctx->swapchain_stale_w = width;
            ctx->swapchain_stale_h = height;
            ctx->swapchain_retry = 0;
        }
        vo_state_unlock(ctx);
        return AVERROR(EAGAIN);
    }
    ret = resize(renderer, width, height);
    vo_release(ctx);

    return ret;
}

int renderer_release_frames(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx || ctx->quiesced) {
        return 1;
    }
    if (!vo_borrow(ctx, VO_DRAIN_WAIT_MS)) {
        return 0;
    }
    gpu_quiesce(ctx);
    vo_release(ctx);

    return 1;
}

void renderer_quiesce(Renderer *renderer, int drain_gpu) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx || ctx->quiesced) {
        return;
    }
    if (!vo_stop(ctx)) {
        return;
    }
    ctx->quiesced = 1;
    if (drain_gpu) {
        gpu_quiesce(ctx);
    } else {
        ctx->gpu_busy = 1;
    }
}

int renderer_destroy(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!renderer) {
        return 1;
    }
    if (!vo_stop(ctx)) {
        return 0;
    }
    destroy(renderer);

    return 1;
}

int renderer_list_gpu_devices(void) {
    int listed = 0;

#if LACHESIS_HAVE_D3D11
    {
        IDXGIFactory1 *factory = dxgi_open_factory();

        if (factory) {
            IDXGIAdapter1 *adapters[MAX_GPU_DEVICES] = {0};
            enum GpuClass classes[MAX_GPU_DEVICES];
            GpuDeviceNames names;
            int num = dxgi_list_adapters(factory, names, classes, adapters);

            renderer_report_gpu_devices("Direct3D 11", names, num, 0);
            for (int i = 0; i < num; i++) {
                IDXGIAdapter1_Release(adapters[i]);
            }
            IDXGIFactory1_Release(factory);
            listed = 1;
        }
    }
#endif

#if LACHESIS_HAVE_VULKAN
    {
        enum GpuClass classes[MAX_GPU_DEVICES];
        GpuDeviceNames names;
        int num = list_vk_devices_standalone(names, classes);

        if (num >= 0) {
            renderer_report_gpu_devices("Vulkan", names, num, 0);
            listed = 1;
        }
    }
#endif

    if (!listed) {
        return AVERROR_EXTERNAL;
    }

    return 0;
}

unsigned renderer_video_decode_caps(Renderer *renderer) {
#if LACHESIS_HAVE_VULKAN
    if (renderer && renderer->backend == RENDERER_API_VULKAN) {
        return ((RendererContext *)renderer)->decode_caps;
    }
#endif
    (void)renderer;

    return 0;
}

int renderer_maps_hw_frames(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return 0;
    }

#if LACHESIS_HAVE_VULKAN
    if (ctx->api.backend == RENDERER_API_VULKAN) {
        return 1;
    }
#endif
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11) {
        return 1;
    }
#endif

    return ctx->gpu && (ctx->gpu->import_caps.tex & PL_HANDLE_DMA_BUF) != 0;
}

const enum AVPixelFormat *renderer_supported_pixfmts(Renderer *renderer,
                                                     int *count) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx || !ctx->num_pixfmts) {
        *count = 0;
        return NULL;
    }
    *count = ctx->num_pixfmts;

    return ctx->pixfmts;
}

int renderer_max_texture_size(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;
    uint32_t max_dim;

    if (!ctx || !ctx->gpu) {
        return 0;
    }
    max_dim = ctx->gpu->limits.max_tex_2d_dim;

    return max_dim > INT_MAX ? INT_MAX : (int)max_dim;
}

int renderer_is_vsync_blocked(Renderer *renderer) {
    if (!renderer) {
        return 1;
    }

    switch (renderer->backend) {
#if LACHESIS_HAVE_VULKAN
    case RENDERER_API_VULKAN:
        switch (((RendererContext *)renderer)->present_mode) {
        case VK_PRESENT_MODE_FIFO_KHR:
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return 1;
        default:
            return 0;
        }
#endif
#if LACHESIS_HAVE_OPENGL
    case RENDERER_API_OPENGL:
        return ((RendererContext *)renderer)->gl_swap_interval > 0;
#endif
    default:
        return 1;
    }
}

int renderer_frame_stats(Renderer *renderer, double *acquire_ms,
                         double *convert_ms, double *render_ms,
                         double *present_ms) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return 0;
    }
    vo_state_lock(ctx);
    if (!ctx->stat_valid) {
        vo_state_unlock(ctx);
        return 0;
    }
    if (acquire_ms) {
        *acquire_ms = ctx->stat_acquire_ms;
    }
    if (convert_ms) {
        *convert_ms = ctx->stat_convert_ms;
    }
    if (render_ms) {
        *render_ms = ctx->stat_render_ms;
    }
    if (present_ms) {
        *present_ms = ctx->stat_present_ms;
    }
    vo_state_unlock(ctx);

    return 1;
}
