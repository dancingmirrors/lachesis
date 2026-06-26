/*
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

#define VK_NO_PROTOTYPES
#define VK_ENABLE_BETA_EXTENSIONS

/* clang-format off */
#include "lachesis_config.h"
#include "lachesis_log.h"
#include "lachesis_renderer.h"
/* clang-format on */

#if LACHESIS_HAVE_LIBPLACEBO
#define HAVE_VULKAN_RENDERER 1
#endif

#if HAVE_VULKAN_RENDERER

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <SDL3/SDL_vulkan.h>

#include <libplacebo/config.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/utils/frame_queue.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
#include <libplacebo/utils/libav.h>
#pragma GCC diagnostic pop
#include <libplacebo/vulkan.h>

static const char sbs360_shader[] =
    "//!PARAM yaw\n"
    "//!DESC Horizontal view angle (degrees, positive = right)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM -180.0\n"
    "//!MAXIMUM 180.0\n"
    "0.0\n"
    "\n"
    "//!PARAM pitch\n"
    "//!DESC Vertical view angle (degrees, positive = up)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM -90.0\n"
    "//!MAXIMUM 90.0\n"
    "0.0\n"
    "\n"
    "//!PARAM hfov\n"
    "//!DESC Horizontal field of view (degrees)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 10.0\n"
    "//!MAXIMUM 170.0\n"
    "90.0\n"
    "\n"
    "//!HOOK MAIN\n"
    "//!BIND HOOKED\n"
    "//!DESC 360 Panini projection (rectilinear when zoomed in)\n"
    "//!WIDTH OUTPUT.w\n"
    "//!HEIGHT OUTPUT.h\n"
    "\n"
    "#define PI 3.14159265358979323846\n"
    "\n"
    "vec4 sample_catmull_rom(vec2 uv) {\n"
    "    vec2 sz  = HOOKED_size;\n"
    "    vec2 sp  = uv * sz;\n"
    "    vec2 tc1 = floor(sp - 0.5) + 0.5;\n"
    "    vec2 f   = sp - tc1;\n"
    "\n"
    "    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "\n"
    "    vec2 w12 = w1 + w2;\n"
    "    vec2 off = w2 / w12;\n"
    "    vec2 t0  = (tc1 - 1.0) / sz;\n"
    "    vec2 t3  = (tc1 + 2.0) / sz;\n"
    "    vec2 t12 = (tc1 + off) / sz;\n"
    "\n"
    "    vec4 r = vec4(0.0);\n"
    "    r += HOOKED_tex(vec2(t0.x,  t0.y )) * w0.x  * w0.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t0.y )) * w12.x * w0.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t0.y )) * w3.x  * w0.y;\n"
    "    r += HOOKED_tex(vec2(t0.x,  t12.y)) * w0.x  * w12.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t12.y)) * w12.x * w12.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t12.y)) * w3.x  * w12.y;\n"
    "    r += HOOKED_tex(vec2(t0.x,  t3.y )) * w0.x  * w3.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t3.y )) * w12.x * w3.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t3.y )) * w3.x  * w3.y;\n"
    "    return r;\n"
    "}\n"
    "\n"
    "vec4 hook() {\n"
    "    vec2 ndc = HOOKED_pos * 2.0 - 1.0;\n"
    "    ndc.y    = -ndc.y;\n"
    "\n"
    "    float aspect   = target_size.x / target_size.y;\n"
    "    float hfov_rad = hfov * (PI / 180.0);\n"
    "    float hh       = hfov_rad * 0.5;\n"
    "    float sh       = sin(hh);\n"
    "    float ch       = cos(hh);\n"
    "\n"
    "    float d = smoothstep(80.0, 160.0, hfov);\n"
    "\n"
    "    float kx = ndc.x * sh / (d + ch);\n"
    "    float ky = ndc.y * sh / ((d + ch) * aspect);\n"
    "    float kk = kx * kx;\n"
    "\n"
    "    float cphi  = (-kk * d + sqrt(1.0 + kk * (1.0 - d * d))) / (1.0 + kk);\n"
    "    float sphi  = kx * (d + cphi);\n"
    "    float tanth = ky * (d + cphi);\n"
    "\n"
    "    float invn  = inversesqrt(1.0 + tanth * tanth);\n"
    "    float costh = invn;\n"
    "    float sinth = tanth * invn;\n"
    "\n"
    "    vec3 ray = vec3(costh * sphi, sinth, costh * cphi);\n"
    "\n"
    "    float p  = pitch * (PI / 180.0);\n"
    "    float cp = cos(p), sp = sin(p);\n"
    "    mat3 Rx  = mat3(\n"
    "        1.0, 0.0,  0.0,\n"
    "        0.0,  cp,  -sp,\n"
    "        0.0,  sp,   cp\n"
    "    );\n"
    "\n"
    "    float ya = yaw * (PI / 180.0);\n"
    "    float cy = cos(ya), sy = sin(ya);\n"
    "    mat3 Ry  = mat3(\n"
    "        cy, 0.0, -sy,\n"
    "        0.0, 1.0, 0.0,\n"
    "        sy, 0.0, cy\n"
    "    );\n"
    "\n"
    "    vec3 dir = Ry * Rx * ray;\n"
    "\n"
    "    float lon = atan(dir.x, dir.z);\n"
    "    float lat = asin(clamp(dir.y, -1.0, 1.0));\n"
    "    float u   = lon / (2.0 * PI) + 0.5;\n"
    "    float v   = 0.5 - lat / PI;\n"
    "\n"
#if 0
    "    return sample_catmull_rom(vec2(u, v));\n"
#endif
    "    return HOOKED_tex(vec2(u, v));\n"
    "}\n";

#include <libavutil/bprint.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavutil/version.h>

#define LACHESIS_HAVE_PL_CACHE 1
#include <libplacebo/cache.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define LACHESIS_PATH_SEP "\\"
#else
#define LACHESIS_PATH_SEP "/"
#endif
#define LACHESIS_SHADER_CACHE_LIMIT (64u << 20)

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#elif defined(__linux__) || defined(__GLIBC__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <link.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#endif

#ifndef FF_API_VULKAN_SYNC_QUEUES
#define FF_API_VULKAN_SYNC_QUEUES (LIBAVUTIL_VERSION_MAJOR < 61)
#endif

#ifndef FF_DISABLE_DEPRECATION_WARNINGS
#if defined(_MSC_VER)
#define FF_DISABLE_DEPRECATION_WARNINGS __pragma(warning(push)) __pragma(warning(disable : 4996))
#define FF_ENABLE_DEPRECATION_WARNINGS __pragma(warning(pop))
#else
#define FF_DISABLE_DEPRECATION_WARNINGS \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define FF_ENABLE_DEPRECATION_WARNINGS _Pragma("GCC diagnostic pop")
#endif
#endif

#endif

struct VkRenderer {
    const AVClass *class;

    int (*create)(VkRenderer *renderer, SDL_Window *window, AVDictionary *dict);

    int (*get_hw_dev)(VkRenderer *renderer, AVBufferRef **dev);

    int (*display)(VkRenderer *renderer, AVFrame *frame, RenderParams *params);

    int (*resize)(VkRenderer *renderer, int width, int height);

    void (*destroy)(VkRenderer *renderer);
};

#if HAVE_VULKAN_RENDERER

typedef struct RendererContext {
    VkRenderer api;

    pl_vk_inst placebo_instance;
    pl_vulkan placebo_vulkan;
    pl_swapchain swapchain;
    VkSurfaceKHR vk_surface;
    pl_renderer renderer;
    pl_tex tex[4];

    pl_log vk_log;

    AVBufferRef *hw_device_ref;
    AVBufferRef *hw_frame_ref;
    enum AVPixelFormat *transfer_formats;
    AVHWFramesConstraints *constraints;
    unsigned decode_caps;

    PFN_vkGetInstanceProcAddr get_proc_addr;
    VkInstance inst;

    AVFrame *vk_frame;

    const struct pl_hook *sbs360_hook;
    int sbs360_enabled;
    float sbs360_yaw;
    float sbs360_pitch;
    float sbs360_hfov;

    int benchmark;

    pl_tex osd_tex;

#if LACHESIS_HAVE_PL_CACHE
    pl_cache shader_cache;
    char *cache_path;
#endif
} RendererContext;

static inline int enable_debug(const AVDictionary *opt) {
    AVDictionaryEntry *entry = av_dict_get(opt, "debug", NULL, 0);
    int debug = entry && strtol(entry->value, NULL, 10);
    return debug;
}

static void hwctx_lock_queue(void *priv, uint32_t qf, uint32_t qidx) {
    AVHWDeviceContext *avhwctx = priv;
    const AVVulkanDeviceContext *hwctx = avhwctx->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
    FF_DISABLE_DEPRECATION_WARNINGS
    hwctx->lock_queue(avhwctx, qf, qidx);
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static void hwctx_unlock_queue(void *priv, uint32_t qf, uint32_t qidx) {
    AVHWDeviceContext *avhwctx = priv;
    const AVVulkanDeviceContext *hwctx = avhwctx->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
    FF_DISABLE_DEPRECATION_WARNINGS
    hwctx->unlock_queue(avhwctx, qf, qidx);
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static int add_instance_extension(const char **ext, unsigned num_ext,
                                  const AVDictionary *opt,
                                  AVDictionary **dict) {
    const char *inst_ext_key = "instance_extensions";
    AVDictionaryEntry *entry;
    AVBPrint buf;
    char *ext_list = NULL;
    int ret;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (int i = 0; i < num_ext; i++) {
        if (i) {
            av_bprintf(&buf, "+%s", ext[i]);
        } else {
            av_bprintf(&buf, "%s", ext[i]);
        }
    }

    entry = av_dict_get(opt, inst_ext_key, NULL, 0);
    if (entry && entry->value && entry->value[0]) {
        if (num_ext) {
            av_bprintf(&buf, "+");
        }
        av_bprintf(&buf, "%s", entry->value);
    }

    ret = av_bprint_finalize(&buf, &ext_list);
    if (ret < 0) {
        return ret;
    }
    return av_dict_set(dict, inst_ext_key, ext_list, AV_DICT_DONT_STRDUP_VAL);
}

static int add_device_extension(const AVDictionary *opt,
                                AVDictionary **dict) {
    const char *dev_ext_key = "device_extensions";
    AVDictionaryEntry *entry;
    AVBPrint buf;
    char *ext_list = NULL;
    int ret;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "%s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++) {
        av_bprintf(&buf, "+%s", pl_vulkan_recommended_extensions[i]);
    }

    entry = av_dict_get(opt, dev_ext_key, NULL, 0);
    if (entry && entry->value && entry->value[0]) {
        av_bprintf(&buf, "+%s", entry->value);
    }

    ret = av_bprint_finalize(&buf, &ext_list);
    if (ret < 0) {
        return ret;
    }
    return av_dict_set(dict, dev_ext_key, ext_list, AV_DICT_DONT_STRDUP_VAL);
}

static const char *select_device(const AVDictionary *opt) {
    const AVDictionaryEntry *entry;

    entry = av_dict_get(opt, "device", NULL, 0);
    if (entry) {
        return entry->value;
    }
    return NULL;
}

static int create_vk_by_placebo(VkRenderer *renderer,
                                const char **ext, unsigned num_ext,
                                const AVDictionary *opt);

static int create_vk_by_hwcontext(VkRenderer *renderer,
                                  const char **ext, unsigned num_ext,
                                  const AVDictionary *opt) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *dev;
    AVVulkanDeviceContext *hwctx;
    AVDictionary *dict = NULL;
    int ret;

    ret = add_instance_extension(ext, num_ext, opt, &dict);
    if (ret < 0) {
        return ret;
    }
    ret = add_device_extension(opt, &dict);
    if (ret) {
        av_dict_free(&dict);
        return ret;
    }

    ret = av_hwdevice_ctx_create(&ctx->hw_device_ref, AV_HWDEVICE_TYPE_VULKAN,
                                 select_device(opt), dict, 0);
    av_dict_free(&dict);
    if (ret < 0) {
        return ret;
    }

    dev = (AVHWDeviceContext *)ctx->hw_device_ref->data;
    hwctx = dev->hwctx;

    if (hwctx->get_proc_addr != (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr()) {
        /* XXX */
        return AVERROR_PATCHWELCOME;
    }

    ctx->get_proc_addr = hwctx->get_proc_addr;
    ctx->inst = hwctx->inst;

    struct pl_vulkan_import_params import_params = {
        .instance = hwctx->inst,
        .get_proc_addr = hwctx->get_proc_addr,
        .phys_device = hwctx->phys_dev,
        .device = hwctx->act_dev,
        .extensions = hwctx->enabled_dev_extensions,
        .num_extensions = hwctx->nb_enabled_dev_extensions,
        .features = &hwctx->device_features,
        .lock_queue = hwctx_lock_queue,
        .unlock_queue = hwctx_unlock_queue,
        .queue_ctx = dev,
        .queue_graphics = {
            .index = VK_QUEUE_FAMILY_IGNORED,
            .count = 0,
        },
        .queue_compute = {
            .index = VK_QUEUE_FAMILY_IGNORED,
            .count = 0,
        },
        .queue_transfer = {
            .index = VK_QUEUE_FAMILY_IGNORED,
            .count = 0,
        },
    };
    for (int i = 0; i < hwctx->nb_qf; i++) {
        const AVVulkanDeviceQueueFamily *qf = &hwctx->qf[i];

        if (qf->flags & VK_QUEUE_GRAPHICS_BIT) {
            import_params.queue_graphics.index = qf->idx;
            import_params.queue_graphics.count = qf->num;
        }
        if (qf->flags & VK_QUEUE_COMPUTE_BIT) {
            import_params.queue_compute.index = qf->idx;
            import_params.queue_compute.count = qf->num;
        }
        if (qf->flags & VK_QUEUE_TRANSFER_BIT) {
            import_params.queue_transfer.index = qf->idx;
            import_params.queue_transfer.count = qf->num;
        }
    }

#if defined(VK_KHR_internally_synchronized_queues) && PL_API_VER >= 365
    for (unsigned i = 0; i < hwctx->nb_enabled_dev_extensions; i++) {
        if (!strcmp(hwctx->enabled_dev_extensions[i],
                    VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME)) {
            import_params.queue_graphics.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
            import_params.queue_compute.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
            import_params.queue_transfer.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
            import_params.lock_queue = NULL;
            import_params.unlock_queue = NULL;
            break;
        }
    }
#elif defined(VK_KHR_internally_synchronized_queues)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
    log_warn("VK_KHR_internally_synchronized_queues with libplacebo < 365 hack.\n");
#endif
    for (unsigned i = 0; i < hwctx->nb_enabled_dev_extensions; i++) {
        if (!strcmp(hwctx->enabled_dev_extensions[i],
                    VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME)) {
            av_buffer_unref(&ctx->hw_device_ref);
            ctx->inst = NULL;
            return create_vk_by_placebo(renderer, ext, num_ext, opt);
        }
    }
#endif
    ctx->placebo_vulkan = pl_vulkan_import(ctx->vk_log, &import_params);
    if (!ctx->placebo_vulkan) {
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static void placebo_lock_queue(struct AVHWDeviceContext *dev_ctx,
                               uint32_t queue_family, uint32_t index) {
    RendererContext *ctx = dev_ctx->user_opaque;
    pl_vulkan vk = ctx->placebo_vulkan;
#if FF_API_VULKAN_SYNC_QUEUES
    FF_DISABLE_DEPRECATION_WARNINGS
    vk->lock_queue(vk, queue_family, index);
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static void placebo_unlock_queue(struct AVHWDeviceContext *dev_ctx,
                                 uint32_t queue_family,
                                 uint32_t index) {
    RendererContext *ctx = dev_ctx->user_opaque;
    pl_vulkan vk = ctx->placebo_vulkan;
#if FF_API_VULKAN_SYNC_QUEUES
    FF_DISABLE_DEPRECATION_WARNINGS
    vk->unlock_queue(vk, queue_family, index);
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static int get_decode_queue(VkRenderer *renderer, int *index, int *count) {
    RendererContext *ctx = (RendererContext *)renderer;
    VkQueueFamilyProperties *queue_family_prop = NULL;
    uint32_t num_queue_family_prop = 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_prop;
    PFN_vkGetInstanceProcAddr get_proc_addr = ctx->get_proc_addr;

    *index = -1;
    *count = 0;
    get_queue_family_prop = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        get_proc_addr(ctx->placebo_instance->instance,
                      "vkGetPhysicalDeviceQueueFamilyProperties");
    get_queue_family_prop(ctx->placebo_vulkan->phys_device,
                          &num_queue_family_prop, NULL);
    if (!num_queue_family_prop) {
        return AVERROR_EXTERNAL;
    }

    queue_family_prop = av_calloc(num_queue_family_prop,
                                  sizeof(*queue_family_prop));
    if (!queue_family_prop) {
        return AVERROR(ENOMEM);
    }

    get_queue_family_prop(ctx->placebo_vulkan->phys_device,
                          &num_queue_family_prop,
                          queue_family_prop);

    for (int i = 0; i < num_queue_family_prop; i++) {
        if (queue_family_prop[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            *index = i;
            *count = queue_family_prop[i].queueCount;
            break;
        }
    }
    av_free(queue_family_prop);

    return 0;
}

static int create_vk_by_placebo(VkRenderer *renderer,
                                const char **ext, unsigned num_ext,
                                const AVDictionary *opt) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *device_ctx;
    AVVulkanDeviceContext *vk_dev_ctx;
    int decode_index;
    int decode_count;
    int ret;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    const char **dev_exts;
    int num_dev_exts;
#endif

    ctx->get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();

    ctx->placebo_instance = pl_vk_inst_create(ctx->vk_log, pl_vk_inst_params(.get_proc_addr = ctx->get_proc_addr, .debug = enable_debug(opt), .extensions = ext, .num_extensions = num_ext));
    if (!ctx->placebo_instance) {
        return AVERROR_EXTERNAL;
    }
    ctx->inst = ctx->placebo_instance->instance;

    /* clang-format off */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    dev_exts = av_vk_get_optional_device_extensions(&num_dev_exts);
    if (!dev_exts) {
        return AVERROR(ENOMEM);
    }
#endif
    ctx->placebo_vulkan = pl_vulkan_create(ctx->vk_log,
                                           pl_vulkan_params(
                                               .instance = ctx->placebo_instance->instance,
                                               .get_proc_addr = ctx->placebo_instance->get_proc_addr,
                                               .surface = ctx->vk_surface,
                                               .allow_software = false,
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
                                               .opt_extensions = dev_exts,
                                               .num_opt_extensions = num_dev_exts,
#endif
                                               .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                               .device_name = select_device(opt), ));
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    av_free(dev_exts);
#endif
    /* clang-format on */
    if (!ctx->placebo_vulkan) {
        return AVERROR_EXTERNAL;
    }
    ctx->hw_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ctx->hw_device_ref) {
        return AVERROR(ENOMEM);
    }

    device_ctx = (AVHWDeviceContext *)ctx->hw_device_ref->data;
    device_ctx->user_opaque = ctx;

    vk_dev_ctx = device_ctx->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
    FF_DISABLE_DEPRECATION_WARNINGS
#if defined(VK_KHR_internally_synchronized_queues) && PL_API_VER >= 365
    {
        int isq = 0;
        for (int i = 0; i < ctx->placebo_vulkan->num_extensions; i++) {
            if (!strcmp(ctx->placebo_vulkan->extensions[i],
                        VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME)) {
                isq = 1;
                break;
            }
        }
        if (!isq) {
            vk_dev_ctx->lock_queue = placebo_lock_queue;
            vk_dev_ctx->unlock_queue = placebo_unlock_queue;
        }
    }
#else
    vk_dev_ctx->lock_queue = placebo_lock_queue;
    vk_dev_ctx->unlock_queue = placebo_unlock_queue;
#endif
    FF_ENABLE_DEPRECATION_WARNINGS
#endif

    vk_dev_ctx->get_proc_addr = ctx->placebo_instance->get_proc_addr;

    vk_dev_ctx->inst = ctx->placebo_instance->instance;
    vk_dev_ctx->phys_dev = ctx->placebo_vulkan->phys_device;
    vk_dev_ctx->act_dev = ctx->placebo_vulkan->device;

    vk_dev_ctx->device_features = *ctx->placebo_vulkan->features;

    vk_dev_ctx->enabled_inst_extensions = ctx->placebo_instance->extensions;
    vk_dev_ctx->nb_enabled_inst_extensions = ctx->placebo_instance->num_extensions;

    vk_dev_ctx->enabled_dev_extensions = ctx->placebo_vulkan->extensions;
    vk_dev_ctx->nb_enabled_dev_extensions = ctx->placebo_vulkan->num_extensions;

    int nb_qf = 0;
    vk_dev_ctx->qf[nb_qf] = (AVVulkanDeviceQueueFamily){
        .idx = ctx->placebo_vulkan->queue_graphics.index,
        .num = ctx->placebo_vulkan->queue_graphics.count,
        .flags = VK_QUEUE_GRAPHICS_BIT,
    };
    nb_qf++;
    vk_dev_ctx->qf[nb_qf] = (AVVulkanDeviceQueueFamily){
        .idx = ctx->placebo_vulkan->queue_transfer.index,
        .num = ctx->placebo_vulkan->queue_transfer.count,
        .flags = VK_QUEUE_TRANSFER_BIT,
    };
    nb_qf++;
    vk_dev_ctx->qf[nb_qf] = (AVVulkanDeviceQueueFamily){
        .idx = ctx->placebo_vulkan->queue_compute.index,
        .num = ctx->placebo_vulkan->queue_compute.count,
        .flags = VK_QUEUE_COMPUTE_BIT,
    };
    nb_qf++;
    ret = get_decode_queue(renderer, &decode_index, &decode_count);
    if (ret < 0) {
        return ret;
    }

    vk_dev_ctx->qf[nb_qf] = (AVVulkanDeviceQueueFamily){
        .idx = decode_index,
        .num = decode_count,
        .flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
    };

    nb_qf++;
    vk_dev_ctx->nb_qf = nb_qf;

    ret = av_hwdevice_ctx_init(ctx->hw_device_ref);
    if (ret < 0) {
        return ret;
    }

    ctx->decode_caps = 0;
    for (int i = 0; i < ctx->placebo_vulkan->num_extensions; i++) {
        const char *ext = ctx->placebo_vulkan->extensions[i];
        if (!strcmp(ext, "VK_KHR_video_decode_h264")) {
            ctx->decode_caps |= VK_DECODE_CAP_H264;
        } else if (!strcmp(ext, "VK_KHR_video_decode_h265")) {
            ctx->decode_caps |= VK_DECODE_CAP_HEVC;
        } else if (!strcmp(ext, "VK_KHR_video_decode_av1")) {
            ctx->decode_caps |= VK_DECODE_CAP_AV1;
        } else if (!strcmp(ext, "VK_KHR_video_decode_vp9")) {
            ctx->decode_caps |= VK_DECODE_CAP_VP9;
        }
    }

    return 0;
}

static VkPresentModeKHR select_present_mode(RendererContext *ctx, const char *name) {
    static const struct {
        const char *name;
        VkPresentModeKHR mode;
    } map[] = {
        {"fifo", VK_PRESENT_MODE_FIFO_KHR},
        {"fifo-relaxed", VK_PRESENT_MODE_FIFO_RELAXED_KHR},
        {"mailbox", VK_PRESENT_MODE_MAILBOX_KHR},
        {"immediate", VK_PRESENT_MODE_IMMEDIATE_KHR},
    };
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    int found = 0;
    for (size_t i = 0; i < FF_ARRAY_ELEMS(map); i++) {
        if (!strcmp(name, map[i].name)) {
            want = map[i].mode;
            found = 1;
            break;
        }
    }
    if (!found) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkPresentModeKHR prefs[3];
    int n = 0;
    prefs[n++] = want;
    if (want == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        prefs[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    prefs[n++] = VK_PRESENT_MODE_FIFO_KHR;

    if (want == VK_PRESENT_MODE_FIFO_KHR) {
        return want;
    }

    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_modes =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
            ctx->get_proc_addr(ctx->inst,
                               "vkGetPhysicalDeviceSurfacePresentModesKHR");
    uint32_t num_modes = 0;
    VkPresentModeKHR *modes = NULL;
    if (get_modes) {
        get_modes(ctx->placebo_vulkan->phys_device, ctx->vk_surface,
                  &num_modes, NULL);
    }
    if (num_modes && (modes = av_calloc(num_modes, sizeof(*modes)))) {
        get_modes(ctx->placebo_vulkan->phys_device, ctx->vk_surface,
                  &num_modes, modes);
    } else {
        num_modes = 0;
    }

    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    for (int p = 0; p < n; p++) {
        int supported = prefs[p] == VK_PRESENT_MODE_FIFO_KHR;
        for (uint32_t i = 0; !supported && i < num_modes; i++) {
            supported = modes[i] == prefs[p];
        }
        if (supported) {
            chosen = prefs[p];
            break;
        }
    }
    av_free(modes);

    return chosen;
}

av_unused static long libplacebo_soversion(const char *path) {
    const char *base;
    const char *stem;

    if (!path || !*path) {
        return -1;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
#ifdef _WIN32
    {
        const char *bslash = strrchr(base, '\\');
        if (bslash) {
            base = bslash + 1;
        }
    }
#endif

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

#ifdef LACHESIS_CAN_ITERATE_LIBS
#define LACHESIS_MAX_PLACEBO_LIBS 8

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

#if LACHESIS_HAVE_PL_CACHE
static int lachesis_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static void lachesis_mkdir_p(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/'
#ifdef _WIN32
            || *p == '\\'
#endif
        ) {
            char c = *p;
            *p = '\0';
            lachesis_mkdir(path);
            *p = c;
        }
    }
    lachesis_mkdir(path);
}

static int resolve_cache_dir(const AVDictionary *opt, char *buf, size_t size) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "cache_dir", NULL, 0);
    const char *base;

    if (entry && entry->value && entry->value[0]) {
        snprintf(buf, size, "%s", entry->value);
        return 0;
    }
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base && base[0]) {
        snprintf(buf, size, "%s\\lachesis", base);
        return 0;
    }
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/Library/Caches/lachesis", base);
        return 0;
    }
#else
    base = getenv("XDG_CACHE_HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/lachesis", base);
        return 0;
    }
    base = getenv("HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/.cache/lachesis", base);
        return 0;
    }
#endif
    return -1;
}

/* Any failure simply leaves rendering uncached. */
static void cache_setup(RendererContext *ctx, const AVDictionary *opt) {
    char dir[4096];
    const AVDictionaryEntry *entry = av_dict_get(opt, "cache", NULL, 0);
    int enabled = entry && entry->value ? strtol(entry->value, NULL, 10) : 1;
    size_t need;
    FILE *f;

    if (!enabled || !ctx->placebo_vulkan) {
        return;
    }
    if (resolve_cache_dir(opt, dir, sizeof(dir)) < 0) {
        return;
    }
    lachesis_mkdir_p(dir);

    need = strlen(dir) + strlen(LACHESIS_PATH_SEP "shaders.bin") + 1;
    ctx->cache_path = av_malloc(need);
    if (!ctx->cache_path) {
        return;
    }
    snprintf(ctx->cache_path, need, "%s%s", dir, LACHESIS_PATH_SEP "shaders.bin");

    ctx->shader_cache = pl_cache_create(pl_cache_params(
            .log = ctx->vk_log,
            .max_total_size = LACHESIS_SHADER_CACHE_LIMIT));
    if (!ctx->shader_cache) {
        av_freep(&ctx->cache_path);
        return;
    }

    f = fopen(ctx->cache_path, "rb");
    if (f) {
        pl_cache_load_file(ctx->shader_cache, f);
        fclose(f);
    }

    pl_gpu_set_cache(ctx->placebo_vulkan->gpu, ctx->shader_cache);
}

static void cache_save(RendererContext *ctx) {
    FILE *f;

    if (!ctx->shader_cache || !ctx->cache_path) {
        return;
    }
    f = fopen(ctx->cache_path, "wb");
    if (f) {
        pl_cache_save_file(ctx->shader_cache, f);
        fclose(f);
    }
}
#endif /* LACHESIS_HAVE_PL_CACHE */

static int create(VkRenderer *renderer, SDL_Window *window, AVDictionary *opt) {
    int ret = 0;
    unsigned num_ext = 0;
    const char **ext = NULL;
    int w, h;
    struct pl_log_params vk_log_params = {
        .log_level = enable_debug(opt) ? PL_LOG_DEBUG : PL_LOG_WARN,
        .log_priv = renderer,
    };
    RendererContext *ctx = (RendererContext *)renderer;
    AVDictionaryEntry *entry;

    check_libplacebo_consistency();

    ctx->vk_log = pl_log_create(PL_API_VER, &vk_log_params);

    entry = av_dict_get(opt, "benchmark", NULL, 0);
    ctx->benchmark = entry && strtol(entry->value, NULL, 10);

    {
        Uint32 sdl_num_ext = 0;
        char const *const *sdl_ext =
            SDL_Vulkan_GetInstanceExtensions(&sdl_num_ext);
        if (!sdl_ext) {
            return AVERROR_EXTERNAL;
        }

        num_ext = sdl_num_ext;
        ext = av_calloc(num_ext, sizeof(*ext));
        if (!ext) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        memcpy(ext, sdl_ext, num_ext * sizeof(*ext));
    }

    entry = av_dict_get(opt, "create_by_placebo", NULL, 0);
    if (entry && strtol(entry->value, NULL, 10)) {
        ret = create_vk_by_placebo(renderer, ext, num_ext, opt);
    } else {
        ret = create_vk_by_hwcontext(renderer, ext, num_ext, opt);
    }
    if (ret < 0) {
        goto out;
    }

    if (!SDL_Vulkan_CreateSurface(window, ctx->inst, NULL, &ctx->vk_surface)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    entry = av_dict_get(opt, "present_mode", NULL, 0);
    if (entry && entry->value && *entry->value) {
        present_mode = select_present_mode(ctx, entry->value);
    }

    ctx->swapchain = pl_vulkan_create_swapchain(
        ctx->placebo_vulkan,
        pl_vulkan_swapchain_params(
                .surface = ctx->vk_surface,
                .present_mode = present_mode));
    if (!ctx->swapchain) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

    SDL_GetWindowSizeInPixels(window, &w, &h);
    pl_swapchain_resize(ctx->swapchain, &w, &h);

#if LACHESIS_HAVE_PL_CACHE
    cache_setup(ctx, opt);
#endif

    ctx->renderer = pl_renderer_create(ctx->vk_log, ctx->placebo_vulkan->gpu);
    if (!ctx->renderer) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }

    ctx->vk_frame = av_frame_alloc();
    if (!ctx->vk_frame) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    ret = 0;

out:
    av_free(ext);
    return ret;
}

static int get_hw_dev(VkRenderer *renderer, AVBufferRef **dev) {
    RendererContext *ctx = (RendererContext *)renderer;

    *dev = ctx->hw_device_ref;
    return 0;
}

static int create_hw_frame(VkRenderer *renderer, AVFrame *frame) {
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
        return ret;
    }

    av_hwframe_transfer_get_formats(ctx->hw_frame_ref,
                                    AV_HWFRAME_TRANSFER_DIRECTION_TO,
                                    &ctx->transfer_formats, 0);

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
    int ret = av_frame_copy_props(ctx->vk_frame, frame);
    if (ret < 0) {
        return ret;
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->vk_frame);
    return 0;
}

static int map_frame(VkRenderer *renderer, AVFrame *frame, int use_hw_frame) {
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

static int transfer_frame(VkRenderer *renderer, AVFrame *frame, int use_hw_frame) {
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

static int convert_frame(VkRenderer *renderer, AVFrame *frame) {
    int ret;

    if (!frame->hw_frames_ctx) {
        return 0;
    }

    if (frame->format == AV_PIX_FMT_VULKAN) {
        return 0;
    }

    ret = create_hw_frame(renderer, frame);
    if (ret < 0) {
        return ret;
    }

    for (int use_hw = 1; use_hw >= 0; use_hw--) {
        ret = map_frame(renderer, frame, use_hw);
        if (!ret) {
            return 0;
        }
        if (ret != AVERROR(ENOSYS)) {
            return ret;
        }

        ret = transfer_frame(renderer, frame, use_hw);
        if (!ret) {
            return 0;
        }
        if (ret != AVERROR(ENOSYS)) {
            return ret;
        }
    }

    return ret;
}

static int display(VkRenderer *renderer, AVFrame *frame, RenderParams *params) {
    SDL_Rect *rect = &params->target_rect;
    struct pl_swapchain_frame swap_frame = {0};
    struct pl_frame pl_frame = {0};
    struct pl_frame target = {0};
    RendererContext *ctx = (RendererContext *)renderer;
    struct pl_render_params pl_params = {
        .upscaler = &pl_filter_bilinear,
        .downscaler = &pl_filter_bilinear,
        .sigmoid_params = ctx->benchmark ? NULL : pl_render_default_params.sigmoid_params,
        .color_adjustment = pl_render_default_params.color_adjustment,
        .dither_params = ctx->benchmark ? NULL : pl_render_default_params.dither_params,
        .cone_params = pl_render_default_params.cone_params,
        .color_map_params = pl_render_default_params.color_map_params,
        .disable_linear_scaling = ctx->benchmark || params->disable_linear_scaling,
        .skip_anti_aliasing = ctx->benchmark || params->skip_anti_aliasing,
        .preserve_mixing_cache = ctx->benchmark || params->preserve_mixing_cache,
    };
    int ret = 0;
    struct pl_color_space hint = {0};

    ret = convert_frame(renderer, frame);
    if (ret < 0) {
        return ret;
    }

    if (!pl_map_avframe_ex(ctx->placebo_vulkan->gpu, &pl_frame, pl_avframe_params(.frame = frame, .tex = ctx->tex))) {
        return AVERROR_EXTERNAL;
    }

    pl_color_space_from_avframe(&hint, frame);
    pl_swapchain_colorspace_hint(ctx->swapchain, &hint);

    static int64_t t_acq, t_rnd, t_prs, t_n;
    int64_t _ts0 = av_gettime_relative();

    if (!pl_swapchain_start_frame(ctx->swapchain, &swap_frame)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    int64_t _ts1 = av_gettime_relative();
    t_acq += _ts1 - _ts0;

    pl_frame_from_swapchain(&target, &swap_frame);

    target.crop = (pl_rect2df){.x0 = rect->x, .x1 = rect->x + rect->w, .y0 = rect->y, .y1 = rect->y + rect->h};
    switch (params->video_background_type) {
    case VIDEO_BACKGROUND_TILES:
        pl_params.background = PL_CLEAR_TILES;
        pl_params.tile_size = VIDEO_BACKGROUND_TILE_SIZE * 2;
        break;
    case VIDEO_BACKGROUND_COLOR:
        pl_params.background = PL_CLEAR_COLOR;
        for (int i = 0; i < 3; i++) {
            pl_params.background_color[i] = params->video_background_color[i] / 255.0;
        }
        pl_params.background_transparency = (255 - params->video_background_color[3]) / 255.0;
        break;
    case VIDEO_BACKGROUND_NONE:
        pl_frame.repr.alpha = PL_ALPHA_NONE;
        break;
    }

    if (ctx->sbs360_enabled && ctx->sbs360_hook) {
        for (int i = 0; i < ctx->sbs360_hook->num_parameters; i++) {
            struct pl_hook_par *par = (struct pl_hook_par *)&ctx->sbs360_hook->parameters[i];
            if (!strcmp(par->name, "yaw")) {
                par->data->f = ctx->sbs360_yaw;
            }
            if (!strcmp(par->name, "pitch")) {
                par->data->f = ctx->sbs360_pitch;
            }
            if (!strcmp(par->name, "hfov")) {
                par->data->f = ctx->sbs360_hfov;
            }
        }
        pl_params.hooks = &ctx->sbs360_hook;
        pl_params.num_hooks = 1;
    }

    struct pl_overlay osd_overlay;
    struct pl_overlay_part osd_part;
    if (params->osd_pixels && params->osd_width > 0 && params->osd_height > 0) {
        bool tex_ok = ctx->osd_tex &&
            (int)ctx->osd_tex->params.w == params->osd_width &&
            (int)ctx->osd_tex->params.h == params->osd_height;
        if (!tex_ok) {
            pl_tex_destroy(ctx->placebo_vulkan->gpu, &ctx->osd_tex);
            pl_fmt fmt = pl_find_named_fmt(ctx->placebo_vulkan->gpu, "rgba8");
            if (fmt) {
                struct pl_tex_params tp = {
                    .w = params->osd_width,
                    .h = params->osd_height,
                    .format = fmt,
                    .sampleable = true,
                    .host_writable = true,
                };
                ctx->osd_tex = pl_tex_create(ctx->placebo_vulkan->gpu, &tp);
            }
        }
        if (ctx->osd_tex) {
            struct pl_tex_transfer_params xfer = {
                .tex = ctx->osd_tex,
                .ptr = params->osd_pixels,
                .row_pitch = params->osd_stride,
            };
            pl_tex_upload(ctx->placebo_vulkan->gpu, &xfer);
            osd_part = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->osd_width, .y1 = (float)params->osd_height},
                .dst = {.x0 = 0, .y0 = 0, .x1 = (float)params->osd_width, .y1 = (float)params->osd_height},
            };
            osd_overlay = (struct pl_overlay){
                .tex = ctx->osd_tex,
                .mode = PL_OVERLAY_NORMAL,
                .coords = PL_OVERLAY_COORDS_DST_FRAME,
                .repr = {
                    .sys = PL_COLOR_SYSTEM_RGB,
                    .levels = PL_COLOR_LEVELS_FULL,
                    .alpha = PL_ALPHA_INDEPENDENT,
                },
                .color = pl_color_space_srgb,
                .parts = &osd_part,
                .num_parts = 1,
            };
            target.overlays = &osd_overlay;
            target.num_overlays = 1;
        }
    }

    int64_t _ts2 = av_gettime_relative();
    if (!pl_render_image(ctx->renderer, &pl_frame, &target, &pl_params)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    int64_t _ts3 = av_gettime_relative();
    t_rnd += _ts3 - _ts2;

    if (!pl_swapchain_submit_frame(ctx->swapchain)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    pl_swapchain_swap_buffers(ctx->swapchain);
    t_prs += av_gettime_relative() - _ts3;

    if (ctx->benchmark) {
        if (++t_n >= 120) {
            printf("acquire=%.2fms render=%.2fms present=%.2fms\n", t_acq / 1000.0 / t_n, t_rnd / 1000.0 / t_n, t_prs / 1000.0 / t_n);
            t_acq = t_rnd = t_prs = t_n = 0;
        }
    }

out:
    pl_unmap_avframe(ctx->placebo_vulkan->gpu, &pl_frame);
    return ret;
}

static int resize(VkRenderer *renderer, int width, int height) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!pl_swapchain_resize(ctx->swapchain, &width, &height)) {
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static void destroy(VkRenderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;

    av_frame_free(&ctx->vk_frame);
    av_freep(&ctx->transfer_formats);
    av_hwframe_constraints_free(&ctx->constraints);
    av_buffer_unref(&ctx->hw_frame_ref);

    if (ctx->sbs360_hook) {
        pl_mpv_user_shader_destroy(&ctx->sbs360_hook);
    }

    if (ctx->placebo_vulkan) {
#if LACHESIS_HAVE_PL_CACHE
        cache_save(ctx);
        pl_gpu_set_cache(ctx->placebo_vulkan->gpu, NULL);
        pl_cache_destroy(&ctx->shader_cache);
        av_freep(&ctx->cache_path);
#endif
        pl_tex_destroy(ctx->placebo_vulkan->gpu, &ctx->osd_tex);
        for (int i = 0; i < FF_ARRAY_ELEMS(ctx->tex); i++) {
            pl_tex_destroy(ctx->placebo_vulkan->gpu, &ctx->tex[i]);
        }
        pl_renderer_destroy(&ctx->renderer);
        pl_swapchain_destroy(&ctx->swapchain);
        pl_vulkan_destroy(&ctx->placebo_vulkan);
    }

    if (ctx->vk_surface) {
        vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)
                                  ctx->get_proc_addr(ctx->inst, "vkDestroySurfaceKHR");
        vkDestroySurfaceKHR(ctx->inst, ctx->vk_surface, NULL);
        ctx->vk_surface = VK_NULL_HANDLE;
    }

    av_buffer_unref(&ctx->hw_device_ref);
    pl_vk_inst_destroy(&ctx->placebo_instance);

    pl_log_destroy(&ctx->vk_log);
}

static const AVClass vulkan_renderer_class = {
    .class_name = "Vulkan Renderer",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

VkRenderer *vk_get_renderer(void) {
    RendererContext *ctx = av_mallocz(sizeof(*ctx));
    VkRenderer *renderer;

    if (!ctx) {
        return NULL;
    }

    renderer = &ctx->api;
    renderer->class = &vulkan_renderer_class;
    renderer->get_hw_dev = get_hw_dev;
    renderer->create = create;
    renderer->display = display;
    renderer->resize = resize;
    renderer->destroy = destroy;

    return renderer;
}

#else

VkRenderer *vk_get_renderer(void) {
    return NULL;
}

#endif

int vk_renderer_enable_360sbs(VkRenderer *renderer, int enable) {
#if HAVE_VULKAN_RENDERER
    RendererContext *ctx = (RendererContext *)renderer;
    if (enable && !ctx->sbs360_hook) {
        ctx->sbs360_hook = pl_mpv_user_shader_parse(ctx->placebo_vulkan->gpu,
                                                    sbs360_shader,
                                                    sizeof(sbs360_shader) - 1);
        if (!ctx->sbs360_hook) {
            return AVERROR_EXTERNAL;
        }
        ctx->sbs360_yaw = 0.0f;
        ctx->sbs360_pitch = 0.0f;
        ctx->sbs360_hfov = 90.0f;
    } else if (!enable && ctx->sbs360_hook) {
        pl_mpv_user_shader_destroy(&ctx->sbs360_hook);
    }
    ctx->sbs360_enabled = enable;
    return 0;
#else
    return AVERROR(ENOSYS);
#endif
}

void vk_renderer_update_360sbs(VkRenderer *renderer, float yaw, float pitch, float hfov) {
#if HAVE_VULKAN_RENDERER
    RendererContext *ctx = (RendererContext *)renderer;
    ctx->sbs360_yaw = yaw;
    ctx->sbs360_pitch = pitch;
    ctx->sbs360_hfov = hfov;
#endif
}

int vk_renderer_create(VkRenderer *renderer, SDL_Window *window,
                       AVDictionary *opt) {
    return renderer->create(renderer, window, opt);
}

int vk_renderer_get_hw_dev(VkRenderer *renderer, AVBufferRef **dev) {
    return renderer->get_hw_dev(renderer, dev);
}

int vk_renderer_display(VkRenderer *renderer, AVFrame *frame, RenderParams *render_params) {
    return renderer->display(renderer, frame, render_params);
}

int vk_renderer_resize(VkRenderer *renderer, int width, int height) {
    return renderer->resize(renderer, width, height);
}

void vk_renderer_destroy(VkRenderer *renderer) {
    renderer->destroy(renderer);
}

unsigned vk_renderer_video_decode_caps(VkRenderer *renderer) {
#if HAVE_VULKAN_RENDERER
    if (!renderer) {
        return 0;
    }
    return ((RendererContext *)renderer)->decode_caps;
#else
    (void)renderer;
    return 0;
#endif
}
