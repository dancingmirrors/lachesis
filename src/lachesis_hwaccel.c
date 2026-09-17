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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

#include "lachesis_alloc.h"
#include "lachesis_config.h"
#include "lachesis_hwaccel.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_renderer.h"

#define HWACCEL_MAX_GPU_NODES 8

typedef struct HwaccelGpuNode {
    char path[64];
    char driver[32];
    int is_renderer;
} HwaccelGpuNode;

static int gpu_nodes(HwaccelGpuNode *nodes, int max);

int hwaccel_glob_match(const char *pattern, const char *text) {
    const char *star = NULL;
    const char *retry = text;

    while (*text) {
        if (*pattern == '?' || av_tolower(*pattern) == av_tolower(*text)) {
            pattern++;
            text++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (star) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') {
        pattern++;
    }

    return !*pattern;
}

#ifdef LACHESIS_HAVE_DRM_NODES

#include <dirent.h>
#include <unistd.h>

static int read_sysfs_line(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    size_t len;

    if (!f) {
        return AVERROR(ENOENT);
    }
    if (!fgets(buf, (int)size, f)) {
        fclose(f);
        return AVERROR(EIO);
    }
    fclose(f);

    len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    return len ? 0 : AVERROR_INVALIDDATA;
}

static int drm_node_driver(const char *node, char *buf, size_t size) {
    const char *base = strrchr(node, '/');
    char path[128];
    char link[256];
    const char *name;
    ssize_t len;

    buf[0] = '\0';
    base = base ? base + 1 : node;
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", base);

    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0) {
        return AVERROR(ENOSYS);
    }
    link[len] = '\0';

    name = strrchr(link, '/');
    name = name ? name + 1 : link;
    if (!*name) {
        return AVERROR_INVALIDDATA;
    }
    av_strlcpy(buf, name, size);

    return 0;
}

static const char *drm_node_vendor(const char *node) {
    static const struct {
        unsigned id;
        const char *name;
    } vendors[] = {
        {0x1002, "AMD"},
        {0x10de, "NVIDIA"},
        {0x13b5, "ARM"},
        {0x1414, "Microsoft"},
        {0x1af4, "Virtio"},
        {0x5143, "Qualcomm"},
        {0x8086, "Intel"},
    };
    const char *base = strrchr(node, '/');
    char path[128];
    char id[32];
    unsigned vendor;

    base = base ? base + 1 : node;
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", base);
    if (read_sysfs_line(path, id, sizeof(id)) < 0) {
        return NULL;
    }
    vendor = (unsigned)strtoul(id, NULL, 0);

    for (size_t i = 0; i < FF_ARRAY_ELEMS(vendors); i++) {
        if (vendors[i].id == vendor) {
            return vendors[i].name;
        }
    }

    return NULL;
}

static int drm_driver_does_vaapi(const char *driver) {
    static const char *const known[] = {
        "amdgpu",
        "i915",
        "nouveau",
        "radeon",
        "virtio_gpu",
        "xe",
    };

    for (size_t i = 0; driver[0] && i < FF_ARRAY_ELEMS(known); i++) {
        if (!strcmp(driver, known[i])) {
            return 1;
        }
    }

    return 0;
}

static int rank_gpu_node(const HwaccelGpuNode *node) {
    const char *want = renderer_wanted_device();
    char described[128];

    if (node->is_renderer) {
        return 0;
    }
    if (want) {
        const char *vendor = drm_node_vendor(node->path);

        snprintf(described, sizeof(described), "%s%s%s", vendor ? vendor : "",
                 vendor ? " " : "", node->driver);
        if (described[0] &&
            (av_stristr(described, want) ||
             hwaccel_glob_match(want, described))) {
            return 1;
        }
    }

    return drm_driver_does_vaapi(node->driver) ? 2 : 3;
}

static int list_gpu_nodes(const char *own, HwaccelGpuNode *nodes, int max) {
    int ranks[HWACCEL_MAX_GPU_NODES];
    struct dirent *ent;
    DIR *dir;
    int num = 0;

    if (max <= 0) {
        return 0;
    }
    if (max > HWACCEL_MAX_GPU_NODES) {
        max = HWACCEL_MAX_GPU_NODES;
    }

    if (own[0]) {
        av_strlcpy(nodes[0].path, own, sizeof(nodes[0].path));
        if (drm_node_driver(own, nodes[0].driver,
                            sizeof(nodes[0].driver)) < 0) {
            nodes[0].driver[0] = '\0';
        }
        nodes[0].is_renderer = 1;
        num = 1;
    }

    dir = opendir("/dev/dri");
    if (!dir) {
        return num;
    }

    while ((ent = readdir(dir)) && num < max) {
        HwaccelGpuNode *node = &nodes[num];

        if (strncmp(ent->d_name, "renderD", 7)) {
            continue;
        }
        av_strlcpy(node->path, "/dev/dri/", sizeof(node->path));
        av_strlcat(node->path, ent->d_name, sizeof(node->path));
        if (!strcmp(node->path, own)) {
            continue;
        }
        if (drm_node_driver(node->path, node->driver,
                            sizeof(node->driver)) < 0) {
            node->driver[0] = '\0';
        }
        node->is_renderer = 0;
        num++;
    }
    closedir(dir);

    for (int i = 0; i < num; i++) {
        ranks[i] = rank_gpu_node(&nodes[i]);
    }
    for (int i = 1; i < num; i++) {
        HwaccelGpuNode hold = nodes[i];
        int rank = ranks[i];
        int j = i;

        while (j > 0 && ranks[j - 1] > rank) {
            nodes[j] = nodes[j - 1];
            ranks[j] = ranks[j - 1];
            j--;
        }
        nodes[j] = hold;
        ranks[j] = rank;
    }

    return num;
}

static int gpu_nodes(HwaccelGpuNode *nodes, int max) {
    char own[64];

    if (renderer_device_node(renderer, own, sizeof(own)) < 0) {
        own[0] = '\0';
    }

    return list_gpu_nodes(own, nodes, max);
}

#else /* !LACHESIS_HAVE_DRM_NODES */

static int gpu_nodes(HwaccelGpuNode *nodes, int max) {
    (void)nodes;
    (void)max;

    return 0;
}

#endif /* LACHESIS_HAVE_DRM_NODES */

#define LACHESIS_READBACK_ALIGN 64

static int hwdownload_alloc(HwDownload *dl, AVFrame *dst, const AVFrame *src) {
    const AVHWFramesContext *frames =
        (const AVHWFramesContext *)src->hw_frames_ctx->data;
    enum AVPixelFormat *formats;
    int ret;

    if (!dl->pool || dl->width != frames->width ||
        dl->height != frames->height || dl->sw_format != frames->sw_format) {
        ret = av_hwframe_transfer_get_formats(src->hw_frames_ctx,
                                              AV_HWFRAME_TRANSFER_DIRECTION_FROM,
                                              &formats, 0);
        if (ret < 0) {
            return ret;
        }
        ret = formats[0] == AV_PIX_FMT_NONE
            ? AVERROR(ENOSYS)
            : av_image_get_buffer_size(formats[0], frames->width,
                                       frames->height,
                                       LACHESIS_READBACK_ALIGN);
        if (ret < 0) {
            av_freep(&formats);
            return ret;
        }

        av_buffer_pool_uninit(&dl->pool);
        dl->pool = av_buffer_pool_init((size_t)ret, NULL);
        if (!dl->pool) {
            av_freep(&formats);
            return AVERROR(ENOMEM);
        }
        dl->format = formats[0];
        dl->sw_format = frames->sw_format;
        dl->width = frames->width;
        dl->height = frames->height;
        av_freep(&formats);
    }

    dst->format = dl->format;
    dst->width = dl->width;
    dst->height = dl->height;

    dst->buf[0] = av_buffer_pool_get(dl->pool);
    if (!dst->buf[0]) {
        return AVERROR(ENOMEM);
    }

    ret = av_image_fill_arrays(dst->data, dst->linesize, dst->buf[0]->data,
                               dst->format, dst->width, dst->height,
                               LACHESIS_READBACK_ALIGN);
    if (ret < 0) {
        av_buffer_unref(&dst->buf[0]);
        return ret;
    }

    return 0;
}

int hwdownload_frame(HwDownload *dl, AVFrame *dst, const AVFrame *src) {
    int ret;

    av_frame_unref(dst);
    if (hwdownload_alloc(dl, dst, src) < 0) {
        av_frame_unref(dst);
    }
    ret = av_hwframe_transfer_data(dst, src, 0);
    if (ret < 0) {
        return ret;
    }
    dst->width = src->width;
    dst->height = src->height;

    return av_frame_copy_props(dst, src);
}

void hwdownload_free(HwDownload *dl) {
    av_buffer_pool_uninit(&dl->pool);
    memset(dl, 0, sizeof(*dl));
}

enum HwaccelLocality {
    HWACCEL_ON_RENDERER,
    HWACCEL_ON_SAME_GPU,
    HWACCEL_ON_ANY_GPU,
};

typedef struct HwaccelGpus {
    HwaccelGpuNode nodes[HWACCEL_MAX_GPU_NODES];
    int num;
    char own[64];
} HwaccelGpus;

static int hwaccel_takes_node(enum AVHWDeviceType type) {
    return type == AV_HWDEVICE_TYPE_VAAPI || type == AV_HWDEVICE_TYPE_DRM;
}

static void hwaccel_list_gpus(HwaccelGpus *gpus) {
    gpus->own[0] = '\0';
    gpus->num = gpu_nodes(gpus->nodes, FF_ARRAY_ELEMS(gpus->nodes));

    for (int i = 0; i < gpus->num; i++) {
        if (gpus->nodes[i].is_renderer) {
            av_strlcpy(gpus->own, gpus->nodes[i].path, sizeof(gpus->own));
            break;
        }
    }
}

static int hwaccel_would_be_off_gpu(const HwaccelGpus *gpus,
                                    enum AVHWDeviceType type) {
    if (gpus->num <= 1 || !gpus->own[0]) {
        return 0;
    }
    if (hwaccel_takes_node(type)) {
        return 1;
    }

    return type == AV_HWDEVICE_TYPE_CUDA &&
        renderer_api(renderer) == RENDERER_API_VULKAN;
}

static const char *const *vaapi_drivers_for(const char *kernel_driver) {
    static const char *const intel[] = {"iHD", "i965", NULL};
    static const char *const intel_xe[] = {"iHD", NULL};
    static const char *const amd[] = {"radeonsi", NULL};
    static const char *const amd_old[] = {"r600", "radeonsi", NULL};
    static const char *const nvidia_open[] = {"nouveau", NULL};
    static const char *const nvidia[] = {"nvidia", NULL};
    static const struct {
        const char *kernel;
        const char *const *drivers;
    } map[] = {
        {"i915", intel},
        {"xe", intel_xe},
        {"amdgpu", amd},
        {"radeon", amd_old},
        {"nouveau", nvidia_open},
        {"nvidia", nvidia},
        {"nvidia-drm", nvidia},
    };

    if (!kernel_driver || !kernel_driver[0]) {
        return NULL;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(map); i++) {
        if (!strcmp(kernel_driver, map[i].kernel)) {
            return map[i].drivers;
        }
    }

    return NULL;
}

static int open_on_node(AVBufferRef **device_ctx, enum AVHWDeviceType type,
                        const HwaccelGpuNode *node) {
    const char *const *drivers;
    const char *driver_env;
    int ret;

    ret = av_hwdevice_ctx_create(device_ctx, type, node->path, NULL, 0);
    if (ret >= 0 || type != AV_HWDEVICE_TYPE_VAAPI) {
        return ret;
    }
    driver_env = getenv("LIBVA_DRIVER_NAME");
    if (driver_env && driver_env[0]) {
        return ret;
    }

    drivers = vaapi_drivers_for(node->driver);
    for (int i = 0; drivers && drivers[i]; i++) {
        AVDictionary *opts = NULL;
        int err;

        *device_ctx = NULL;
        av_dict_set(&opts, "driver", drivers[i], 0);
        err = av_hwdevice_ctx_create(device_ctx, type, node->path, opts, 0);
        av_dict_free(&opts);
        if (err >= 0) {
            log_verbose("VA-API on %s (%s) only worked with the %s driver "
                        "named explicitly.\n",
                        node->path, node->driver, drivers[i]);
            return err;
        }
        *device_ctx = NULL;
    }

    return ret;
}

static int keep_best_error(int best, int ret) {
    if (!best || (best == AVERROR(ENOSYS) && ret != AVERROR(ENOSYS))) {
        return ret;
    }

    return best;
}

static int try_hwaccel(AVBufferRef **device_ctx, const char *name,
                       enum HwaccelLocality locality, const HwaccelGpus *gpus,
                       int asked_for, int *off_gpu) {
    enum AVHWDeviceType type;
    AVBufferRef *render_dev;
    int ret;

    *off_gpu = 0;

    type = av_hwdevice_find_type_by_name(name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return AVERROR(ENOTSUP);
    }

    switch (locality) {
    case HWACCEL_ON_RENDERER:
        if (renderer_get_hw_dev(renderer, &render_dev) < 0) {
            return AVERROR(ENOSYS);
        }
        return av_hwdevice_ctx_create_derived(device_ctx, type, render_dev, 0);

    case HWACCEL_ON_SAME_GPU:
        if (!hwaccel_takes_node(type) || !gpus->own[0]) {
            return AVERROR(ENOSYS);
        }
        for (int i = 0; i < gpus->num; i++) {
            if (!gpus->nodes[i].is_renderer) {
                continue;
            }
            return open_on_node(device_ctx, type, &gpus->nodes[i]);
        }
        return AVERROR(ENOSYS);

    case HWACCEL_ON_ANY_GPU:
        if (!hwaccel_takes_node(type)) {
            if (!asked_for) {
                return AVERROR(ENOSYS);
            }
            ret = av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0);
            if (ret >= 0) {
                *off_gpu = hwaccel_would_be_off_gpu(gpus, type);
            }
            return ret;
        }
        ret = 0;
        for (int i = 0; i < gpus->num; i++) {
            int err;

            if (gpus->nodes[i].is_renderer) {
                continue;
            }
            err = open_on_node(device_ctx, type, &gpus->nodes[i]);
            if (err >= 0) {
                *off_gpu = gpus->own[0] != '\0';
                return err;
            }
            ret = keep_best_error(ret, err);
            *device_ctx = NULL;
        }
        return ret ? ret : AVERROR(ENOSYS);
    }

    return AVERROR_BUG;
}

static void hwaccel_report_failure(const char *name, const HwaccelGpus *gpus,
                                   int err) {
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
    char tried[256] = "";

    if (!hwaccel_takes_node(type) || !gpus->num) {
        log_dead("hwaccel %s is not available! (%s)\n", name, av_err2str(err));
        return;
    }

    for (int i = 0; i < gpus->num; i++) {
        av_strlcatf(tried, sizeof(tried), "%s%s", i ? ", " : "",
                    gpus->nodes[i].path);
        if (gpus->nodes[i].driver[0]) {
            av_strlcatf(tried, sizeof(tried), " (%s)", gpus->nodes[i].driver);
        }
    }
    log_dead("hwaccel %s is not available on %s! (%s)\n", name, tried,
             av_err2str(err));
}

static int hwaccel_codec_allowed(enum AVCodecID codec_id) {
    const char *list = hwaccel_codecs;
    const char *name = avcodec_get_name(codec_id);
    size_t name_len = strlen(name);
    int allowed = 0;
    int listed = 0;

    if (!list) {
        return 1;
    }

    for (const char *p = list; *p;) {
        int exclude = *p == '-';
        const char *entry = p + exclude;
        size_t len = strcspn(entry, ",");

        if ((len == 3 && !av_strncasecmp(entry, "all", 3)) ||
            (len == name_len && !av_strncasecmp(entry, name, len))) {
            if (exclude) {
                return 0;
            }
            allowed = 1;
        }
        listed |= !exclude;
        p = entry + len;
        p += *p == ',';
    }

    return allowed || (!listed && *list);
}

static int hwaccel_decodes(const AVCodec *codec, enum AVHWDeviceType type) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);

        if (!config) {
            return 0;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == type) {
            return 1;
        }
    }
}

/* For example, libdav1d outranks the native AV1 but decodes in software only. */
static const AVCodec *hwaccel_decoder(const AVCodec *codec,
                                      enum AVHWDeviceType type) {
    void *iter = NULL;
    const AVCodec *cur;

    if (!codec || type == AV_HWDEVICE_TYPE_NONE) {
        return NULL;
    }
    if (hwaccel_decodes(codec, type)) {
        return codec;
    }
    if (video_codec_name) {
        return NULL;
    }

    while ((cur = av_codec_iterate(&iter))) {
        if (cur->id == codec->id && av_codec_is_decoder(cur) &&
            hwaccel_decodes(cur, type)) {
            return cur;
        }
    }

    return NULL;
}

static unsigned decode_cap_for_codec(enum AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return RENDERER_DECODE_CAP_H264;
    case AV_CODEC_ID_HEVC:
        return RENDERER_DECODE_CAP_HEVC;
    case AV_CODEC_ID_AV1:
        return RENDERER_DECODE_CAP_AV1;
    case AV_CODEC_ID_VP9:
        return RENDERER_DECODE_CAP_VP9;
    default:
        return 0;
    }
}

static int vulkan_decodes(enum AVCodecID codec_id) {
    unsigned caps = renderer_video_decode_caps(renderer);
    unsigned bit = decode_cap_for_codec(codec_id);

    return bit ? (caps & bit) != 0 : caps != 0;
}

static double codec_decode_effort(enum AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_VVC:
        return 4.0;
    case AV_CODEC_ID_AV1:
        return 2.5;
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP9:
        return 2.0;
    case AV_CODEC_ID_H264:
        return 1.0;
    case AV_CODEC_ID_VP8:
        return 0.6;
    default:
        return 0.4;
    }
}

#define HWACCEL_OFF_GPU_LOAD 100.0

static double software_decode_load(const AVCodecContext *avctx,
                                   AVRational frame_rate) {
    double pixels = (double)FFMAX(avctx->coded_width, avctx->width) *
        FFMAX(avctx->coded_height, avctx->height);
    double fps = frame_rate.num > 0 && frame_rate.den > 0
        ? av_q2d(frame_rate)
        : 30.0;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    int depth = desc ? desc->comp[0].depth : avctx->bits_per_raw_sample;

    if (pixels <= 0) {
        return HWACCEL_OFF_GPU_LOAD;
    }

    if (fps > 1000.0) {
        fps = 30.0;
    }

    return pixels * fps * codec_decode_effort(avctx->codec_id) *
        (depth > 8 ? 1.4 : 1.0) / 1e6;
}

static int create_hwaccel(AVBufferRef **device_ctx, const AVCodec **codec,
                          const AVCodecContext *avctx, AVRational frame_rate) {
    static const char *auto_hwaccels_vk[] = {
        "vulkan", "vaapi", "videotoolbox", "cuda", "d3d11va", "dxva2", NULL};
    static const char *auto_hwaccels_other[] = {
        "vaapi", "videotoolbox", "d3d11va", "dxva2", "cuda", NULL};
    const char *const *auto_hwaccels =
        renderer_api(renderer) == RENDERER_API_VULKAN ? auto_hwaccels_vk
                                                      : auto_hwaccels_other;
    HwaccelGpus gpus;
    int off_gpu_pays;
    int off_gpu;
    int saved_level;
    int ret;

    *device_ctx = NULL;

    if (no_hwaccel) {
        return 0;
    }

    if (!hwaccel_codec_allowed(avctx->codec_id)) {
        log_verbose("Not using hwaccel for %s.\n",
                    avcodec_get_name(avctx->codec_id));
        return 0;
    }

    hwaccel_list_gpus(&gpus);
    off_gpu_pays =
        software_decode_load(avctx, frame_rate) >= HWACCEL_OFF_GPU_LOAD;

    if (!hwaccel && !off_gpu_pays && !renderer_maps_hw_frames(renderer)) {
        log_verbose("Not using hwaccel: %s cannot take hardware frames "
                    "without a copy back.\n",
                    renderer_api_name(renderer));
        return 0;
    }

    saved_level = av_log_get_level();
    if (saved_level < AV_LOG_VERBOSE) {
        av_log_set_level(AV_LOG_QUIET);
    }

    if (hwaccel) {
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name(hwaccel);
        const AVCodec *hw_codec = hwaccel_decoder(*codec, type);
        int why = 0;

        for (enum HwaccelLocality loc = HWACCEL_ON_RENDERER;
             loc <= HWACCEL_ON_ANY_GPU; loc++) {
            ret = try_hwaccel(device_ctx, hwaccel, loc, &gpus, 1, &off_gpu);
            if (ret >= 0) {
                av_log_set_level(saved_level);
                if (!hw_codec) {
                    av_buffer_unref(device_ctx);
                    log_warn("No decoder for %s can use hwaccel %s. "
                             "Decoding in software.\n",
                             avcodec_get_name(avctx->codec_id), hwaccel);
                    return 0;
                }
                media_info_set_hwaccel(hwaccel, off_gpu);
                *codec = hw_codec;
                return 0;
            }
            why = keep_best_error(why, ret);
            *device_ctx = NULL;
        }
        av_log_set_level(saved_level);
        hwaccel_report_failure(hwaccel, &gpus, why);

        return why;
    }

    for (enum HwaccelLocality loc = HWACCEL_ON_RENDERER;
         loc <= HWACCEL_ON_ANY_GPU; loc++) {
        for (int i = 0; auto_hwaccels[i]; i++) {
            const char *name = auto_hwaccels[i];
            enum AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
            const AVCodec *hw_codec = hwaccel_decoder(*codec, type);

            if (!hw_codec) {
                continue;
            }
            if (type == AV_HWDEVICE_TYPE_VULKAN &&
                (loc != HWACCEL_ON_RENDERER ||
                 !vulkan_decodes(avctx->codec_id))) {
                continue;
            }

            /* XXX */
            if (loc == HWACCEL_ON_ANY_GPU && !off_gpu_pays &&
                hwaccel_would_be_off_gpu(&gpus, type)) {
                continue;
            }

            ret = try_hwaccel(device_ctx, name, loc, &gpus, 0, &off_gpu);
            if (!ret) {
                av_log_set_level(saved_level);
                media_info_set_hwaccel(name, off_gpu);
                *codec = hw_codec;
                return 0;
            }
            *device_ctx = NULL;
        }
    }
    av_log_set_level(saved_level);

    return 0;
}

static int hwaccel_size_usable(AVBufferRef *device_ctx, int width, int height) {
    int max_w = 0, max_h = 0;

    if (hwaccel_max_size < 0 || width <= 0 || height <= 0) {
        return 1;
    }

    if (hwaccel_max_size > 0) {
        max_w = max_h = hwaccel_max_size;
    } else {
        AVHWFramesConstraints *c =
            av_hwdevice_get_hwframe_constraints(device_ctx, NULL);
        if (!c) {
            return 1;
        }
        max_w = c->max_width;
        max_h = c->max_height;
        av_hwframe_constraints_free(&c);
    }

    if ((max_w > 0 && width > max_w) || (max_h > 0 && height > max_h)) {
        char limit[64];

        if (max_w > 0 && max_h > 0) {
            snprintf(limit, sizeof(limit), "%dx%d", max_w, max_h);
        } else if (max_w > 0) {
            snprintf(limit, sizeof(limit), "%d wide", max_w);
        } else {
            snprintf(limit, sizeof(limit), "%d high", max_h);
        }
        log_warn("%dx%d exceeds %s.\n", width, height, limit);
        return 0;
    }

    return 1;
}

static int hwaccel_usable(const AVCodec *codec, const AVBufferRef *device_ctx) {
    const AVHWDeviceContext *dev = (const AVHWDeviceContext *)device_ctx->data;

    return hwaccel_decodes(codec, dev->type);
}

int hwaccel_open_device(AVBufferRef **device_ctx, const AVCodec **codec,
                        const AVCodecContext *avctx, AVRational frame_rate) {
    const AVCodec *sw_codec = *codec;
    int ret = create_hwaccel(device_ctx, codec, avctx, frame_rate);

    if (ret < 0) {
        return ret;
    }
    if (*device_ctx &&
        (!hwaccel_usable(*codec, *device_ctx) ||
         !hwaccel_size_usable(*device_ctx,
                              FFMAX(avctx->coded_width, avctx->width),
                              FFMAX(avctx->coded_height, avctx->height)))) {
        av_buffer_unref(device_ctx);
        media_info_set_hwaccel(NULL, 0);
        *codec = sw_codec;
    }

    return 0;
}
