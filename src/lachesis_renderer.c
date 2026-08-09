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
#include "lachesis_config.h"
#include "lachesis_equalizer.h"
#include "lachesis_icon.h"
#include "lachesis_log.h"
#include "lachesis_renderer.h"
#include "lachesis_view360.h"
/* clang-format on */

#include <limits.h>

#include <libplacebo/config.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/deinterlacing.h>
#include <libplacebo/utils/frame_queue.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
#include <libplacebo/utils/libav.h>
#pragma GCC diagnostic pop

#if LACHESIS_HAVE_VULKAN

#define VK_NO_PROTOTYPES
#define VK_ENABLE_BETA_EXTENSIONS

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <SDL3/SDL_vulkan.h>

#include <libplacebo/vulkan.h>

#include "lachesis_present_vulkan.h"

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

#endif /* LACHESIS_HAVE_VULKAN */

#if LACHESIS_HAVE_OPENGL
#include <libplacebo/opengl.h>
#endif

#if LACHESIS_HAVE_D3D11
#include <d3d10.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libplacebo/d3d11.h>
#endif

#include <libavutil/bprint.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
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

#define LACHESIS_MAX_OVERLAYS 2

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#elif defined(__linux__) || defined(__GLIBC__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <link.h>
#define LACHESIS_CAN_ITERATE_LIBS 1
#endif

struct Renderer {
    const AVClass *class;

    enum RendererApi backend;
};

typedef struct RendererContext {
    Renderer api;

    pl_gpu gpu;
    pl_swapchain swapchain;
    pl_renderer renderer;
    pl_log log_ctx;

    pl_tex tex[4];
    pl_tex prev_tex[4];
    pl_tex next_tex[4];
    AVFrame *deint_prev;
    AVFrame *sw_frame;

    AVBufferRef *hw_device_ref;

#if LACHESIS_HAVE_VULKAN
    pl_vk_inst placebo_instance;
    pl_vulkan placebo_vulkan;
    VkSurfaceKHR vk_surface;

    AVBufferRef *hw_frame_ref;
    enum AVPixelFormat *transfer_formats;
    AVHWFramesConstraints *constraints;
    unsigned decode_caps;
    /* Not necessarily the requested mode. */
    VkPresentModeKHR present_mode;

    PFN_vkGetInstanceProcAddr get_proc_addr;
    VkInstance inst;

    const char *const *dev_extensions;
    int num_dev_extensions;
    const VkPhysicalDeviceFeatures2 *dev_features;

    int present_timing_silent;
    const char **filtered_dev_exts;

    AVFrame *vk_frame;
#endif

#if LACHESIS_HAVE_OPENGL
    pl_opengl placebo_gl;
    SDL_GLContext gl_context;
#endif

#if LACHESIS_HAVE_D3D11
    pl_d3d11 placebo_d3d11;
    ID3D11Texture2D *d3d11_pool;
    pl_tex *d3d11_views;
    unsigned d3d11_num_views;
    int d3d11_planes;
#endif

    /* See build_pixfmt_list(). */
    enum AVPixelFormat *pixfmts;
    int num_pixfmts;

    int zero_copy_failed;

    char api_name[64];
    char device_name[256];

    const struct pl_hook *sbs360_hook;
    int sbs360_enabled;
    float sbs360_yaw;
    float sbs360_pitch;
    float sbs360_roll;
    float sbs360_hfov;
    enum View360Layout sbs360_layout;

    int benchmark;

    double stat_acquire_ms;
    double stat_render_ms;
    double stat_present_ms;
    int stat_valid;

    struct pl_color_space last_hint;
    bool have_hint;

    pl_tex osd_tex;
    pl_tex sub_tex;

    AVFrame *blank_frame;

#if LACHESIS_HAVE_PL_CACHE
    pl_cache shader_cache;
    char *cache_path;
#endif

    void *icc_data;
    size_t icc_len;
    uint64_t icc_sig;
    int icc_from_file;
    int icc_auto;

    int hdr_auto;
    int hdr_warned;
    int have_display_hdr;
    struct pl_hdr_metadata display_hdr;
} RendererContext;

static inline int enable_debug(const AVDictionary *opt) {
    AVDictionaryEntry *entry = av_dict_get(opt, "debug", NULL, 0);
    int debug = entry && strtol(entry->value, NULL, 10);
    return debug;
}

#if LACHESIS_HAVE_VULKAN

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

static int want_host_image_copy(const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "host_image_copy", NULL, 0);
    int want = 0;

    if (entry && entry->value) {
        want = strtol(entry->value, NULL, 10) != 0;
    }

    return want;
}

static const char *const placebo_instance_extensions[] = {
    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
    VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
#ifdef VK_KHR_surface_maintenance1
    VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
#endif
#ifdef VK_EXT_surface_maintenance1
    VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
#endif
};

static int add_instance_extension(const char **ext, unsigned num_ext,
                                  const AVDictionary *opt,
                                  AVDictionary **dict) {
    const char *inst_ext_key = "instance_extensions";
    AVDictionaryEntry *entry;
    AVBPrint buf;
    char *ext_list = NULL;
    int ret;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (unsigned i = 0; i < num_ext; i++) {
        if (buf.len) {
            av_bprintf(&buf, "+");
        }
        av_bprintf(&buf, "%s", ext[i]);
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(placebo_instance_extensions); i++) {
        if (buf.len) {
            av_bprintf(&buf, "+");
        }
        av_bprintf(&buf, "%s", placebo_instance_extensions[i]);
    }

    entry = av_dict_get(opt, inst_ext_key, NULL, 0);
    if (entry && entry->value && entry->value[0]) {
        if (buf.len) {
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
                                AVDictionary **dict, int present_timing) {
    const char *dev_ext_key = "device_extensions";
    AVDictionaryEntry *entry;
    AVBPrint buf;
    char *ext_list = NULL;
    int ret;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "%s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++) {
        if (!want_host_image_copy(opt) &&
            !strcmp(pl_vulkan_recommended_extensions[i],
                    VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) {
            continue;
        }
        av_bprintf(&buf, "+%s", pl_vulkan_recommended_extensions[i]);
    }
    if (present_timing) {
        int num_present_ext = 0;
        const char *const *present_ext =
            vkpresent_device_extensions(&num_present_ext);

        for (int i = 0; i < num_present_ext; i++) {
            av_bprintf(&buf, "+%s", present_ext[i]);
        }
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

static struct {
    PFN_vkGetInstanceProcAddr real_proc_addr;
    PFN_vkEnumerateDeviceExtensionProperties real_enumerate;
    PFN_vkGetPhysicalDeviceFeatures2 real_features2;
    PFN_vkGetPhysicalDeviceFeatures2KHR real_features2_khr;
} no_host_copy;

static void scrub_host_image_copy(VkPhysicalDeviceFeatures2 *features) {
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)features; s;
         s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
            ((VkPhysicalDeviceHostImageCopyFeaturesEXT *)s)->hostImageCopy =
                VK_FALSE;
            break;
#ifdef VK_VERSION_1_4
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
            ((VkPhysicalDeviceVulkan14Features *)s)->hostImageCopy = VK_FALSE;
            break;
#endif
        default:
            break;
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
hide_host_copy_enumerate(VkPhysicalDevice phys_dev, const char *layer,
                         uint32_t *count, VkExtensionProperties *props) {
    VkExtensionProperties *all;
    uint32_t num_all = 0;
    uint32_t kept = 0;
    VkResult ret;

    ret = no_host_copy.real_enumerate(phys_dev, layer, &num_all, NULL);
    if (ret != VK_SUCCESS || !num_all) {
        *count = 0;
        return ret;
    }

    all = av_calloc(num_all, sizeof(*all));
    if (!all) {
        return no_host_copy.real_enumerate(phys_dev, layer, count, props);
    }

    ret = no_host_copy.real_enumerate(phys_dev, layer, &num_all, all);
    if (ret != VK_SUCCESS && ret != VK_INCOMPLETE) {
        av_free(all);
        return ret;
    }

    for (uint32_t i = 0; i < num_all; i++) {
        if (!strcmp(all[i].extensionName,
                    VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) {
            continue;
        }
        all[kept++] = all[i];
    }

    if (!props) {
        *count = kept;
        ret = VK_SUCCESS;
    } else {
        uint32_t num = FFMIN(*count, kept);

        memcpy(props, all, num * sizeof(*props));
        ret = num < kept ? VK_INCOMPLETE : VK_SUCCESS;
        *count = num;
    }
    av_free(all);

    return ret;
}

static VKAPI_ATTR void VKAPI_CALL
hide_host_copy_features(VkPhysicalDevice phys_dev,
                        VkPhysicalDeviceFeatures2 *features) {
    no_host_copy.real_features2(phys_dev, features);
    scrub_host_image_copy(features);
}

static VKAPI_ATTR void VKAPI_CALL
hide_host_copy_features_khr(VkPhysicalDevice phys_dev,
                            VkPhysicalDeviceFeatures2 *features) {
    no_host_copy.real_features2_khr(phys_dev, features);
    scrub_host_image_copy(features);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
hide_host_copy_proc_addr(VkInstance inst, const char *name) {
    PFN_vkVoidFunction real;

    if (!no_host_copy.real_proc_addr) {
        return NULL;
    }
    real = no_host_copy.real_proc_addr(inst, name);
    if (!real || !name) {
        return real;
    }

    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) {
        no_host_copy.real_enumerate =
            (PFN_vkEnumerateDeviceExtensionProperties)real;
        return (PFN_vkVoidFunction)hide_host_copy_enumerate;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) {
        no_host_copy.real_features2 = (PFN_vkGetPhysicalDeviceFeatures2)real;
        return (PFN_vkVoidFunction)hide_host_copy_features;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) {
        no_host_copy.real_features2_khr =
            (PFN_vkGetPhysicalDeviceFeatures2KHR)real;
        return (PFN_vkVoidFunction)hide_host_copy_features_khr;
    }

    return real;
}

static PFN_vkGetInstanceProcAddr
hide_host_image_copy(PFN_vkGetInstanceProcAddr real) {
    if (!real || real == hide_host_copy_proc_addr) {
        return real;
    }
    no_host_copy.real_proc_addr = real;
    log_verbose("Hiding %s from the Vulkan device.\n",
                VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);

    return hide_host_copy_proc_addr;
}

static const char *const *drop_host_image_copy(RendererContext *ctx,
                                               const char *const *exts,
                                               int num_exts, int *out_num) {
    const char **filtered;
    int n = 0;

    *out_num = num_exts;
    for (int i = 0; i < num_exts; i++) {
        if (!strcmp(exts[i], VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) {
            n = 1;
            break;
        }
    }
    if (!n) {
        return exts;
    }

    filtered = av_calloc(num_exts, sizeof(*filtered));
    if (!filtered) {
        return exts;
    }
    n = 0;
    for (int i = 0; i < num_exts; i++) {
        if (strcmp(exts[i], VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) {
            filtered[n++] = exts[i];
        }
    }

    av_free(ctx->filtered_dev_exts);
    ctx->filtered_dev_exts = filtered;
    *out_num = n;
    log_verbose("Withholding %s from libplacebo.\n",
                VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);

    return (const char *const *)filtered;
}

static int create_vk_by_placebo(Renderer *renderer,
                                const char **ext, unsigned num_ext,
                                const AVDictionary *opt, int present_timing);

static int create_vk_by_hwcontext(Renderer *renderer,
                                  const char **ext, unsigned num_ext,
                                  const AVDictionary *opt, int present_timing) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *dev;
    AVVulkanDeviceContext *hwctx;
    AVDictionary *dict = NULL;
    int ret;

    ret = add_instance_extension(ext, num_ext, opt, &dict);
    if (ret < 0) {
        return ret;
    }
    ret = add_device_extension(opt, &dict, present_timing);
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
        av_buffer_unref(&ctx->hw_device_ref);
        ctx->inst = NULL;
        return create_vk_by_placebo(renderer, ext, num_ext, opt, present_timing);
    }

    ctx->get_proc_addr = hwctx->get_proc_addr;
    ctx->inst = hwctx->inst;

    const char *const *import_exts = hwctx->enabled_dev_extensions;
    int num_import_exts = hwctx->nb_enabled_dev_extensions;

    if (!want_host_image_copy(opt)) {
        import_exts = drop_host_image_copy(ctx, import_exts, num_import_exts,
                                           &num_import_exts);
    }

    struct pl_vulkan_import_params import_params = {
        .instance = hwctx->inst,
        .get_proc_addr = present_timing
            ? vkpresent_wrap_proc_addr(hwctx->get_proc_addr)
            : hwctx->get_proc_addr,
        .phys_device = hwctx->phys_dev,
        .device = hwctx->act_dev,
        .extensions = import_exts,
        .num_extensions = num_import_exts,
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
    for (unsigned i = 0; i < (unsigned)hwctx->nb_enabled_dev_extensions; i++) {
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
    for (unsigned i = 0; i < (unsigned)hwctx->nb_enabled_dev_extensions; i++) {
        if (!strcmp(hwctx->enabled_dev_extensions[i],
                    VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME)) {
            av_buffer_unref(&ctx->hw_device_ref);
            ctx->inst = NULL;
            return create_vk_by_placebo(renderer, ext, num_ext, opt,
                                        present_timing);
        }
    }
#endif
    ctx->dev_extensions = import_exts;
    ctx->num_dev_extensions = num_import_exts;
    ctx->dev_features = &hwctx->device_features;

    ctx->placebo_vulkan = pl_vulkan_import(ctx->log_ctx, &import_params);
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

static int get_decode_queue(Renderer *renderer, int *index, int *count) {
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

    for (int i = 0; i < (int)num_queue_family_prop; i++) {
        if (queue_family_prop[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            *index = i;
            *count = queue_family_prop[i].queueCount;
            break;
        }
    }
    av_free(queue_family_prop);

    return 0;
}

static int create_vk_by_placebo(Renderer *renderer,
                                const char **ext, unsigned num_ext,
                                const AVDictionary *opt, int present_timing) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *device_ctx;
    AVVulkanDeviceContext *vk_dev_ctx;
    PFN_vkGetInstanceProcAddr placebo_proc_addr;
    const char **opt_exts = NULL;
    const char **merged_exts = NULL;
    int num_opt_exts = 0;
    int decode_index;
    int decode_count;
    int ret;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    const char **dev_exts;
    int num_dev_exts;
#endif

    ctx->get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    placebo_proc_addr = present_timing
        ? vkpresent_wrap_proc_addr(ctx->get_proc_addr)
        : ctx->get_proc_addr;
    if (!want_host_image_copy(opt)) {
        placebo_proc_addr = hide_host_image_copy(placebo_proc_addr);
    }

    /* clang-format off */
    ctx->placebo_instance = pl_vk_inst_create(ctx->log_ctx, pl_vk_inst_params(
        .get_proc_addr = placebo_proc_addr,
        .debug = enable_debug(opt),
        .extensions = ext,
        .num_extensions = num_ext));
    /* clang-format on */
    if (!ctx->placebo_instance) {
        return AVERROR_EXTERNAL;
    }
    ctx->inst = ctx->placebo_instance->instance;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    dev_exts = av_vk_get_optional_device_extensions(&num_dev_exts);
    if (!dev_exts) {
        return AVERROR(ENOMEM);
    }
    opt_exts = dev_exts;
    num_opt_exts = num_dev_exts;
#endif

    if (present_timing || !want_host_image_copy(opt)) {
        int num_present_ext = 0;
        const char *const *present_ext =
            present_timing ? vkpresent_device_extensions(&num_present_ext) : NULL;
        const char **merged = av_calloc(num_opt_exts + num_present_ext,
                                        sizeof(*merged));
        int keep_host_copy = want_host_image_copy(opt);
        int n = 0;

        if (!merged) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
            av_free(dev_exts);
#endif
            return AVERROR(ENOMEM);
        }
        for (int i = 0; i < num_opt_exts; i++) {
            if (!keep_host_copy &&
                !strcmp(opt_exts[i], VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) {
                continue;
            }
            merged[n++] = opt_exts[i];
        }
        for (int i = 0; i < num_present_ext; i++) {
            merged[n++] = present_ext[i];
        }
        opt_exts = merged;
        merged_exts = merged;
        num_opt_exts = n;
    }

    /* clang-format off */
    ctx->placebo_vulkan = pl_vulkan_create(ctx->log_ctx,
                                           pl_vulkan_params(
                                               .instance = ctx->placebo_instance->instance,
                                               .get_proc_addr = ctx->placebo_instance->get_proc_addr,
                                               .surface = ctx->vk_surface,
                                               .allow_software = true,
                                               .opt_extensions = opt_exts,
                                               .num_opt_extensions = num_opt_exts,
                                               .features = present_timing ? vkpresent_device_features() : NULL,
                                               .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                               .device_name = select_device(opt), ));
    /* clang-format on */
    av_free(merged_exts);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    av_free(dev_exts);
#endif
    if (!ctx->placebo_vulkan) {
        return AVERROR_EXTERNAL;
    }
    ctx->dev_extensions = ctx->placebo_vulkan->extensions;
    ctx->num_dev_extensions = ctx->placebo_vulkan->num_extensions;
    ctx->dev_features = ctx->placebo_vulkan->features;

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

    vk_dev_ctx->get_proc_addr = ctx->get_proc_addr;

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

    if (decode_index >= 0 && decode_count > 0) {
        vk_dev_ctx->qf[nb_qf] = (AVVulkanDeviceQueueFamily){
            .idx = decode_index,
            .num = decode_count,
            .flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        };
        nb_qf++;
    }
    vk_dev_ctx->nb_qf = nb_qf;

    ret = av_hwdevice_ctx_init(ctx->hw_device_ref);
    if (ret < 0) {
        return ret;
    }

    ctx->decode_caps = 0;
    for (int i = 0; i < ctx->placebo_vulkan->num_extensions; i++) {
        const char *ext_name = ctx->placebo_vulkan->extensions[i];
        if (!strcmp(ext_name, "VK_KHR_video_decode_h264")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_H264;
        } else if (!strcmp(ext_name, "VK_KHR_video_decode_h265")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_HEVC;
        } else if (!strcmp(ext_name, "VK_KHR_video_decode_av1")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_AV1;
        } else if (!strcmp(ext_name, "VK_KHR_video_decode_vp9")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_VP9;
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

static int vk_backend_create(RendererContext *ctx, SDL_Window *window,
                             AVDictionary *opt) {
    Renderer *renderer = &ctx->api;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    AVDictionaryEntry *entry;
    unsigned num_ext = 0;
    const char **ext = NULL;
    int present_timing = 1;
    int by_placebo;
    int ret;
    int w, h;

    entry = av_dict_get(opt, "present_timing", NULL, 0);
    if (entry && entry->value && !strtol(entry->value, NULL, 10)) {
        present_timing = 0;
    }

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
            return AVERROR(ENOMEM);
        }

        memcpy(ext, sdl_ext, num_ext * sizeof(*ext));
    }

    entry = av_dict_get(opt, "create_by_placebo", NULL, 0);
    if (entry && entry->value) {
        by_placebo = strtol(entry->value, NULL, 10) != 0;
        if (!by_placebo && !want_host_image_copy(opt)) {
        }
    } else {
        by_placebo = !want_host_image_copy(opt);
    }

    if (by_placebo) {
        ret = create_vk_by_placebo(renderer, ext, num_ext, opt, present_timing);
    } else {
        ret = create_vk_by_hwcontext(renderer, ext, num_ext, opt, present_timing);
    }
    av_free(ext);
    if (ret < 0) {
        return ret;
    }

    if (!SDL_Vulkan_CreateSurface(window, ctx->inst, NULL, &ctx->vk_surface)) {
        return AVERROR_EXTERNAL;
    }

    if (present_timing) {
        vkpresent_attach(ctx->placebo_vulkan->device, ctx->dev_extensions,
                         ctx->num_dev_extensions, ctx->dev_features);
    }

    entry = av_dict_get(opt, "present_mode", NULL, 0);
    if (entry && entry->value && *entry->value) {
        present_mode = select_present_mode(ctx, entry->value);
    }
    ctx->present_mode = present_mode;

    ctx->swapchain = pl_vulkan_create_swapchain(
        ctx->placebo_vulkan,
        pl_vulkan_swapchain_params(
                .surface = ctx->vk_surface,
                .present_mode = present_mode));
    if (!ctx->swapchain) {
        return AVERROR_EXTERNAL;
    }

    ctx->gpu = ctx->placebo_vulkan->gpu;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    pl_swapchain_resize(ctx->swapchain, &w, &h);

    ctx->vk_frame = av_frame_alloc();
    if (!ctx->vk_frame) {
        return AVERROR(ENOMEM);
    }

    snprintf(ctx->api_name, sizeof(ctx->api_name), "Vulkan");

    {
        PFN_vkGetPhysicalDeviceProperties get_props =
            (PFN_vkGetPhysicalDeviceProperties)
                ctx->get_proc_addr(ctx->inst, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties props;

        if (get_props) {
            get_props(ctx->placebo_vulkan->phys_device, &props);
            snprintf(ctx->device_name, sizeof(ctx->device_name), "%s",
                     props.deviceName);
        }
    }

    return 0;
}

static void vk_backend_destroy(RendererContext *ctx) {
    av_frame_free(&ctx->vk_frame);
    av_freep(&ctx->transfer_formats);
    av_hwframe_constraints_free(&ctx->constraints);
    av_buffer_unref(&ctx->hw_frame_ref);

    pl_swapchain_destroy(&ctx->swapchain);
    pl_vulkan_destroy(&ctx->placebo_vulkan);

    if (ctx->vk_surface) {
        SDL_Vulkan_DestroySurface(ctx->inst, ctx->vk_surface, NULL);
        ctx->vk_surface = VK_NULL_HANDLE;
    }

    av_buffer_unref(&ctx->hw_device_ref);
    pl_vk_inst_destroy(&ctx->placebo_instance);
    av_freep(&ctx->filtered_dev_exts);

    vkpresent_shutdown();
}

#endif /* LACHESIS_HAVE_VULKAN */

#if LACHESIS_HAVE_OPENGL

static const struct gl_profile {
    const char *name;
    int profile;
    int major;
    int minor;
    int prefer_egl;
    int angle;
} gl_profiles[] = {
    {"OpenGL 3.3 core (EGL)", SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, 1, 0},
    {"OpenGL 3.3 core", SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, 0, 0},
    {"OpenGL 3.2 core", SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, 0, 0},
    {"OpenGL ES 3.0", SDL_GL_CONTEXT_PROFILE_ES, 3, 0, 1, 0},
    {"OpenGL ES 3.0 (ANGLE)", SDL_GL_CONTEXT_PROFILE_ES, 3, 0, 1, 1},
};

#define GL_NUM_PROFILES ((int)FF_ARRAY_ELEMS(gl_profiles))

static int gl_video_driver_is(const char *name) {
    const char *driver = SDL_GetCurrentVideoDriver();

    return driver && !strcmp(driver, name);
}

static int gl_profile_usable(const struct gl_profile *p) {
    if (p->angle) {
        return gl_video_driver_is("windows");
    }
    if (p->prefer_egl && p->profile != SDL_GL_CONTEXT_PROFILE_ES) {
        return gl_video_driver_is("x11");
    }

    return 1;
}

static int gl_profile_order(int *order, const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "gles", NULL, 0);
    int es_first = entry && entry->value && strtol(entry->value, NULL, 10);
    int n = 0;

    for (int pass = 0; pass < 2; pass++) {
        int want_es = pass == (es_first ? 0 : 1);

        for (int i = 0; i < GL_NUM_PROFILES; i++) {
            const struct gl_profile *p = &gl_profiles[i];

            if ((p->profile == SDL_GL_CONTEXT_PROFILE_ES) != want_es) {
                continue;
            }
            if (gl_profile_usable(p)) {
                order[n++] = i;
            }
        }
    }

    return n;
}

static int gl_num_attempts(const AVDictionary *opt) {
    int order[GL_NUM_PROFILES];

    return gl_profile_order(order, opt);
}

static const char *gl_apply_profile_hints(int attempt, const AVDictionary *opt) {
    int order[GL_NUM_PROFILES];
    const struct gl_profile *p;

    gl_profile_order(order, opt);
    p = &gl_profiles[order[attempt]];

    if (!SDL_getenv(SDL_HINT_VIDEO_FORCE_EGL)) {
        SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, p->prefer_egl ? "1" : "0");
    }
    if (!SDL_getenv(SDL_HINT_OPENGL_ES_DRIVER)) {
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, p->angle ? "1" : "0");
    }

    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, p->profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, p->major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, p->minor);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    /* No destination alpha. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    return p->name;
}

static pl_voidfunc_t gl_get_proc_addr(const char *name) {
    return (pl_voidfunc_t)SDL_GL_GetProcAddress(name);
}

static void gl_swap_buffers(void *priv) {
    SDL_GL_SwapWindow(priv);
}

static int gl_backend_create(RendererContext *ctx, SDL_Window *window,
                             AVDictionary *opt) {
    AVDictionaryEntry *entry;
    SDL_EGLDisplay egl_display;
    int max_glsl = 0;
    int w, h;

    ctx->gl_context = SDL_GL_CreateContext(window);
    if (!ctx->gl_context) {
        log_verbose("Failed to create a GL context: %s.\n", SDL_GetError());
        return AVERROR_EXTERNAL;
    }
    if (!SDL_GL_MakeCurrent(window, ctx->gl_context)) {
        return AVERROR_EXTERNAL;
    }

    entry = av_dict_get(opt, "max_glsl_version", NULL, 0);
    if (entry && entry->value) {
        max_glsl = (int)strtol(entry->value, NULL, 10);
    }

    entry = av_dict_get(opt, "present_mode", NULL, 0);
    if (entry && entry->value && !strcmp(entry->value, "immediate")) {
        SDL_GL_SetSwapInterval(0);
    } else if (!SDL_GL_SetSwapInterval(1)) {
        SDL_GL_SetSwapInterval(-1);
    }

    egl_display = SDL_EGL_GetCurrentDisplay();
    if (!egl_display) {
        SDL_ClearError();
    }

    /* clang-format off */
    ctx->placebo_gl = pl_opengl_create(ctx->log_ctx,
                                       pl_opengl_params(
                                           .get_proc_addr = gl_get_proc_addr,
                                           .debug = enable_debug(opt),
                                           .allow_software = true,
                                           .max_glsl_version = max_glsl,
                                           .egl_display = egl_display, ));
    if (!ctx->placebo_gl && egl_display) {
        egl_display = NULL;
        ctx->placebo_gl = pl_opengl_create(ctx->log_ctx,
                                           pl_opengl_params(
                                               .get_proc_addr = gl_get_proc_addr,
                                               .debug = enable_debug(opt),
                                               .allow_software = true,
                                               .max_glsl_version = max_glsl, ));
    }
    /* clang-format on */
    if (!ctx->placebo_gl) {
        return AVERROR_EXTERNAL;
    }

    ctx->gpu = ctx->placebo_gl->gpu;

    /* clang-format off */
    ctx->swapchain = pl_opengl_create_swapchain(ctx->placebo_gl,
                                                pl_opengl_swapchain_params(
                                                    .swap_buffers = gl_swap_buffers,
                                                    .framebuffer.flipped = false,
                                                    .priv = window, ));
    /* clang-format on */
    if (!ctx->swapchain) {
        return AVERROR_EXTERNAL;
    }

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        w = h = 1;
    }
    if (!pl_swapchain_resize(ctx->swapchain, &w, &h)) {
        return AVERROR_EXTERNAL;
    }

    snprintf(ctx->api_name, sizeof(ctx->api_name), "OpenGL%s %d.%d",
             ctx->placebo_gl->gpu->glsl.gles ? " ES" : "",
             ctx->placebo_gl->major, ctx->placebo_gl->minor);

    {
        enum { LACHESIS_GL_RENDERER = 0x1F01 };
#ifdef _WIN32
        typedef const unsigned char *(__stdcall * gl_get_string_fn)(unsigned);
#else
        typedef const unsigned char *(*gl_get_string_fn)(unsigned);
#endif
        gl_get_string_fn get_string =
            (gl_get_string_fn)SDL_GL_GetProcAddress("glGetString");
        const unsigned char *name = get_string ? get_string(LACHESIS_GL_RENDERER) : NULL;

        if (name) {
            snprintf(ctx->device_name, sizeof(ctx->device_name), "%s", name);
        }
    }

    if (!(ctx->gpu->import_caps.tex & PL_HANDLE_DMA_BUF)) {
        log_verbose("OpenGL: no DMA-BUF import (EGL display: %s, "
                    "GL_EXT_EGL_image_storage: %s, "
                    "GL_OES_EGL_image_external: %s, "
                    "EGL_EXT_image_dma_buf_import: %s). Hardware frames will "
                    "be copied through system memory.\n",
                    egl_display ? "yes" : "no",
                    pl_opengl_has_ext(ctx->placebo_gl, "GL_EXT_EGL_image_storage") ? "yes" : "no",
                    pl_opengl_has_ext(ctx->placebo_gl, "GL_OES_EGL_image_external") ? "yes" : "no",
                    pl_opengl_has_ext(ctx->placebo_gl, "EGL_EXT_image_dma_buf_import") ? "yes" : "no");
    }

    return 0;
}

static void gl_backend_destroy(RendererContext *ctx) {
    pl_swapchain_destroy(&ctx->swapchain);
    pl_opengl_destroy(&ctx->placebo_gl);

    if (ctx->gl_context) {
        SDL_GL_DestroyContext(ctx->gl_context);
        ctx->gl_context = NULL;
    }
}

#endif /* LACHESIS_HAVE_OPENGL */

#if LACHESIS_HAVE_D3D11

static int force_software(const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "software", NULL, 0);
    int force = 0;

    if (entry && entry->value) {
        force = strtol(entry->value, NULL, 10) != 0;
    }

    return force;
}

static void d3d11_read_device_name(RendererContext *ctx) {
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;

    if (FAILED(ID3D11Device_QueryInterface(ctx->placebo_d3d11->device,
                                           &IID_IDXGIDevice,
                                           (void **)&dxgi_dev))) {
        return;
    }
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, ctx->device_name,
                            (int)sizeof(ctx->device_name), NULL, NULL);
    }
    if (adapter) {
        IDXGIAdapter_Release(adapter);
    }
    IDXGIDevice_Release(dxgi_dev);
}

static void d3d11_protect_device(RendererContext *ctx) {
    ID3D10Multithread *multithread = NULL;

    if (FAILED(ID3D11Device_QueryInterface(ctx->placebo_d3d11->device,
                                           &IID_ID3D10Multithread,
                                           (void **)&multithread))) {
        return;
    }
    ID3D10Multithread_SetMultithreadProtected(multithread, TRUE);
    ID3D10Multithread_Release(multithread);
}

static int d3d11_create_hw_device(RendererContext *ctx) {
    AVD3D11VADeviceContext *hwctx;
    AVHWDeviceContext *dev;
    int ret;

    ctx->hw_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ctx->hw_device_ref) {
        return AVERROR(ENOMEM);
    }

    dev = (AVHWDeviceContext *)ctx->hw_device_ref->data;
    hwctx = dev->hwctx;

    ID3D11Device_AddRef(ctx->placebo_d3d11->device);
    hwctx->device = ctx->placebo_d3d11->device;
    hwctx->BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    ret = av_hwdevice_ctx_init(ctx->hw_device_ref);
    if (ret < 0) {
        av_buffer_unref(&ctx->hw_device_ref);
        return ret;
    }

    return 0;
}

static int d3d11_backend_create(RendererContext *ctx, SDL_Window *window,
                                AVDictionary *opt) {
    HWND hwnd;
    int software = force_software(opt);
    int w, h;

    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                        NULL);
    if (!hwnd) {
        return AVERROR_EXTERNAL;
    }

    /* clang-format off */
    ctx->placebo_d3d11 = pl_d3d11_create(ctx->log_ctx,
                                         pl_d3d11_params(
                                             .debug = enable_debug(opt),
                                             .allow_software = true,
                                             .force_software = software,
                                             .flags = software ? 0 : D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                             .min_feature_level = D3D_FEATURE_LEVEL_10_0, ));
    /* clang-format on */
    if (!ctx->placebo_d3d11) {
        return AVERROR_EXTERNAL;
    }

    ctx->gpu = ctx->placebo_d3d11->gpu;
    d3d11_protect_device(ctx);

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        w = h = 1;
    }

    /* clang-format off */
    ctx->swapchain = pl_d3d11_create_swapchain(ctx->placebo_d3d11,
                                               pl_d3d11_swapchain_params(
                                                   .window = hwnd,
                                                   .width = w,
                                                   .height = h, ));
    /* clang-format on */
    if (!ctx->swapchain) {
        return AVERROR_EXTERNAL;
    }

    if (!pl_swapchain_resize(ctx->swapchain, &w, &h)) {
        return AVERROR_EXTERNAL;
    }

    snprintf(ctx->api_name, sizeof(ctx->api_name), "Direct3D 11");
    d3d11_read_device_name(ctx);

    if (!ctx->placebo_d3d11->software) {
        (void)d3d11_create_hw_device(ctx);
    }

    return 0;
}

static int d3d11_plane_view_formats(DXGI_FORMAT packed, DXGI_FORMAT *view) {
    switch (packed) {
    case DXGI_FORMAT_NV12:
        view[0] = DXGI_FORMAT_R8_UNORM;
        view[1] = DXGI_FORMAT_R8G8_UNORM;
        return 2;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        view[0] = DXGI_FORMAT_R16_UNORM;
        view[1] = DXGI_FORMAT_R16G16_UNORM;
        return 2;
    default:
        return 0;
    }
}

static void d3d11_drop_views(RendererContext *ctx) {
    for (unsigned i = 0; i < ctx->d3d11_num_views; i++) {
        pl_tex_destroy(ctx->gpu, &ctx->d3d11_views[i]);
    }
    av_freep(&ctx->d3d11_views);
    ctx->d3d11_num_views = 0;
    ctx->d3d11_pool = NULL;
}

static bool map_d3d11_frame(RendererContext *ctx, const AVFrame *frame,
                            struct pl_frame *out) {
    const AVHWFramesContext *hwfc =
        (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(hwfc->sw_format);
    ID3D11Texture2D *texture = (ID3D11Texture2D *)frame->data[0];
    unsigned slice = (unsigned)(intptr_t)frame->data[1];
    DXGI_FORMAT view_fmt[4];
    D3D11_TEXTURE2D_DESC tex_desc;
    unsigned need;
    int planes;

    if (!texture || !desc) {
        return false;
    }

    ID3D11Texture2D_GetDesc(texture, &tex_desc);
    planes = d3d11_plane_view_formats(tex_desc.Format, view_fmt);
    if (!planes || slice >= tex_desc.ArraySize) {
        return false;
    }

    need = tex_desc.ArraySize * (unsigned)planes;
    if (ctx->d3d11_pool != texture || ctx->d3d11_num_views != need) {
        d3d11_drop_views(ctx);
        ctx->d3d11_views = av_calloc(need, sizeof(*ctx->d3d11_views));
        if (!ctx->d3d11_views) {
            return false;
        }
        ctx->d3d11_num_views = need;
        ctx->d3d11_planes = planes;
        ctx->d3d11_pool = texture;
    }

    pl_frame_from_avframe(out, frame);
    if (out->num_planes != planes) {
        return false;
    }

    for (int i = 0; i < planes; i++) {
        pl_tex *slot = &ctx->d3d11_views[slice * (unsigned)planes + i];

        if (!*slot) {
            int full_w = (int)tex_desc.Width;
            int full_h = (int)tex_desc.Height;
            int sub_w = i ? desc->log2_chroma_w : 0;
            int sub_h = i ? desc->log2_chroma_h : 0;

            /* clang-format off */
            *slot = pl_d3d11_wrap(ctx->gpu,
                                  pl_d3d11_wrap_params(
                                      .tex = (ID3D11Resource *)texture,
                                      .array_slice = (int)slice,
                                      .fmt = view_fmt[i],
                                      .w = AV_CEIL_RSHIFT(full_w, sub_w),
                                      .h = AV_CEIL_RSHIFT(full_h, sub_h), ));
            /* clang-format on */
            if (!*slot) {
                return false;
            }
            /* A pool allocated without D3D11_BIND_SHADER_RESOURCE wraps happily but can't be sampled. */
            if (!(*slot)->params.sampleable) {
                pl_tex_destroy(ctx->gpu, slot);
                return false;
            }
        }
        out->planes[i].texture = *slot;
    }

    out->repr.bits.color_depth = desc->comp[0].depth;
    out->repr.bits.bit_shift = FFMAX(desc->comp[0].shift, 0);
    out->repr.bits.sample_depth =
        out->planes[0].texture->params.format->component_depth[0];

    if (desc->log2_chroma_w || desc->log2_chroma_h) {
        pl_frame_set_chroma_location(out, pl_chroma_from_av(frame->chroma_location));
    }

    return true;
}

static void d3d11_backend_destroy(RendererContext *ctx) {
    d3d11_drop_views(ctx);
    av_buffer_unref(&ctx->hw_device_ref);
    pl_swapchain_destroy(&ctx->swapchain);
    pl_d3d11_destroy(&ctx->placebo_d3d11);
}

#endif /* LACHESIS_HAVE_D3D11 */

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
    const char *leaf = ctx->api.backend == RENDERER_API_OPENGL
        ? LACHESIS_PATH_SEP "shaders-OpenGL.bin"
        : ctx->api.backend == RENDERER_API_D3D11
        ? LACHESIS_PATH_SEP "shaders-D3D11.bin"
        : LACHESIS_PATH_SEP "shaders-Vulkan.bin";
    size_t need;
    FILE *f;

    if (!enabled || !ctx->gpu) {
        return;
    }
    if (resolve_cache_dir(opt, dir, sizeof(dir)) < 0) {
        return;
    }
    lachesis_mkdir_p(dir);

    need = strlen(dir) + strlen(leaf) + 1;
    ctx->cache_path = av_malloc(need);
    if (!ctx->cache_path) {
        return;
    }
    snprintf(ctx->cache_path, need, "%s%s", dir, leaf);

    ctx->shader_cache = pl_cache_create(pl_cache_params(
            .log = ctx->log_ctx,
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

    pl_gpu_set_cache(ctx->gpu, ctx->shader_cache);
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

static int icc_adopt(RendererContext *ctx, void *data, size_t len) {
    struct pl_icc_profile profile;

    if (!data || !len) {
        av_free(data);
        return 0;
    }

    profile = (struct pl_icc_profile){.data = data, .len = len};
    pl_icc_profile_compute_signature(&profile);

    if (ctx->icc_data && ctx->icc_sig == profile.signature) {
        av_free(data);
        return 0;
    }

    av_freep(&ctx->icc_data);
    ctx->icc_data = data;
    ctx->icc_len = len;
    ctx->icc_sig = profile.signature;

    return 1;
}

static int icc_load_file(RendererContext *ctx, const char *path) {
    long size;
    void *data;
    FILE *f;

    f = fopen(path, "rb");
    if (!f) {
        log_warn("Failed to open ICC profile '%s'.\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    data = av_malloc((size_t)size);
    if (!data) {
        fclose(f);
        return 0;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        av_free(data);
        fclose(f);
        return 0;
    }
    fclose(f);

    if (!icc_adopt(ctx, data, (size_t)size)) {
        return 0;
    }
    log_info("Loaded ICC profile: %s\n", path);

    return 1;
}

static int icc_load_display(RendererContext *ctx, SDL_Window *window) {
    size_t size = 0;
    void *sdl_data;
    void *data;

    if (!ctx->icc_auto || ctx->icc_from_file || !window) {
        return 0;
    }

    sdl_data = SDL_GetWindowICCProfile(window, &size);
    if (!sdl_data || !size) {
        SDL_free(sdl_data);
        if (!ctx->icc_data) {
            return 0;
        }
        av_freep(&ctx->icc_data);
        ctx->icc_len = 0;
        ctx->icc_sig = 0;
        log_verbose("The display advertises no ICC profile.\n");
        return 1;
    }
    data = av_memdup(sdl_data, size);
    SDL_free(sdl_data);

    if (!icc_adopt(ctx, data, size)) {
        return 0;
    }
    log_verbose("Using the display's ICC profile (%llu bytes).\n",
                (unsigned long long)size);

    return 1;
}

static void icc_setup(RendererContext *ctx, SDL_Window *window,
                      const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "icc_profile", NULL, 0);

    if (entry && entry->value && entry->value[0]) {
        ctx->icc_from_file = 1;
        icc_load_file(ctx, entry->value);
        return;
    }

    entry = av_dict_get(opt, "icc_auto", NULL, 0);
    ctx->icc_auto = entry && strtol(entry->value, NULL, 10);
    icc_load_display(ctx, window);
}

#define SDL_SCRGB_NITS 80.0f

static int hdr_refresh(RendererContext *ctx, SDL_Window *window) {
    struct pl_hdr_metadata hdr = {0};
    SDL_PropertiesID props;
    float headroom, sdr_white, max_luma;

    if (!ctx->hdr_auto || !window) {
        return 0;
    }

    props = SDL_GetWindowProperties(window);
    if (!props) {
        return 0;
    }
    if (!SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) {
        goto done;
    }

    headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    if (!(headroom > 1.0f)) {
        goto done;
    }

    sdr_white = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    sdr_white = sdr_white == 1.0f ? PL_COLOR_SDR_WHITE : sdr_white * SDL_SCRGB_NITS;

    max_luma = sdr_white * headroom;
    if (!(max_luma >= 100.0f) || max_luma > 100000.0f) {
        if (!ctx->hdr_warned) {
            ctx->hdr_warned = 1;
            log_warn("Ignoring an implausible display peak of %.1f cd/m².\n", max_luma);
        }
        goto done;
    }
    hdr.max_luma = max_luma;

done:
    if (pl_hdr_metadata_equal(&hdr, &ctx->display_hdr)) {
        return 0;
    }
    ctx->display_hdr = hdr;
    ctx->have_display_hdr = hdr.max_luma > 0;
    if (ctx->have_display_hdr) {
        log_verbose("The display reports a peak of %.1f cd/m².\n", hdr.max_luma);
    } else {
        log_verbose("The display no longer reports HDR metadata.\n");
    }

    return 1;
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

#if LACHESIS_HAVE_PL_CACHE
    cache_setup(ctx, opt);
#endif

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
    static int warned_download;
    int ret;

    if (frame->format == AV_PIX_FMT_VULKAN) {
        return 0;
    }

    create_hw_frame(renderer, frame);

    for (int use_hw = 1; use_hw >= 0; use_hw--) {
        const char *how = "mapping";

        ret = map_frame(renderer, frame, use_hw);
        if (ret) {
            ret = transfer_frame(renderer, frame, use_hw);
            how = "copy";
        }
        if (!ret) {
            if (!use_hw && !warned_download) {
                warned_download = 1;
                log_info("Displaying hardware frames via a system memory %s.\n",
                         how);
            }
            return 0;
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

    av_frame_unref(ctx->sw_frame);
    ret = av_hwframe_transfer_data(ctx->sw_frame, frame, 0);
    if (ret < 0) {
        return ret;
    }
    ret = av_frame_copy_props(ctx->sw_frame, frame);
    if (ret < 0) {
        return ret;
    }

    if (!warned_download) {
        warned_download = 1;
        log_info("Displaying hardware frames via a system memory copy "
                 "(the GPU cannot import %s).\n",
                 av_get_pix_fmt_name(frame->format));
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->sw_frame);

    return 0;
}

static int convert_frame(Renderer *renderer, AVFrame *frame) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!frame->hw_frames_ctx) {
        return 0;
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

static bool map_video_frame(RendererContext *ctx, AVFrame *frame,
                            struct pl_frame *out) {
#if LACHESIS_HAVE_D3D11
    if (ctx->api.backend == RENDERER_API_D3D11 &&
        frame->format == AV_PIX_FMT_D3D11) {
        if (map_d3d11_frame(ctx, frame, out)) {
            return true;
        }
        if (!ctx->zero_copy_failed) {
            ctx->zero_copy_failed = 1;
            log_warn("The GPU rejected a zero copy import of a %s frame. "
                     "Falling back to a system memory copy.\n",
                     av_get_pix_fmt_name(frame->format));
        }
        if (convert_frame_readback(ctx, frame) < 0) {
            return false;
        }
    }
#endif

    if (pl_map_avframe_ex(ctx->gpu, out,
                          pl_avframe_params(.frame = frame, .tex = ctx->tex))) {
        return true;
    }

    if (!frame->hw_frames_ctx) {
        return false;
    }
    if (!ctx->zero_copy_failed) {
        ctx->zero_copy_failed = 1;
        log_warn("The GPU rejected a zero copy import of a %s frame. Falling "
                 "back to a system memory copy.\n",
                 av_get_pix_fmt_name(frame->format));
    }
    if (convert_frame_readback(ctx, frame) < 0) {
        return false;
    }

    return pl_map_avframe_ex(ctx->gpu, out,
                             pl_avframe_params(.frame = frame, .tex = ctx->tex));
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
    return a->data[0] && a->data[0] == b->data[0];
}

static const struct pl_frame *map_deint_ref(RendererContext *ctx, pl_tex *tex,
                                            struct pl_frame *out,
                                            const struct pl_frame *cur,
                                            AVFrame *frame, const AVFrame *self) {
    if (!frame || frames_alias(frame, self)) {
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

static void apply_deinterlace(struct pl_frame *pl_frame,
                              struct pl_render_params *pl_params,
                              const AVFrame *frame, const RenderParams *params) {
    static const struct pl_deinterlace_params yadif = {
        .algo = PL_DEINTERLACE_YADIF,
    };
    static const struct pl_deinterlace_params bob = {
        .algo = PL_DEINTERLACE_BOB,
    };

    switch (params->deinterlace) {
    case DEINTERLACE_YADIF:
        pl_params->deinterlace_params = &yadif;
        break;
    case DEINTERLACE_BOB:
        pl_params->deinterlace_params = &bob;
        break;
    default:
        return;
    }

    enum pl_field first;
    if (!(frame->flags & AV_FRAME_FLAG_INTERLACED)) {
        first = PL_FIELD_TOP;
    } else {
        /* See libplacebo's validate_structs() if there are mysterious failures. */
        first = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? PL_FIELD_TOP : PL_FIELD_BOTTOM;
    }

    pl_frame->field = first;
    pl_frame->first_field = first;
}

static void clip_crops_to_target(struct pl_frame *image, struct pl_frame *target,
                                 pl_rotation rotation) {
    pl_tex tex = target->num_planes > 0 ? target->planes[0].texture : NULL;
    pl_rect2df *dst = &target->crop;
    pl_rect2df *src = &image->crop;
    float dst_w = dst->x1 - dst->x0, dst_h = dst->y1 - dst->y0;
    float src_w = src->x1 - src->x0, src_h = src->y1 - src->y0;
    float sx = src->x0, sy = src->y0;
    float u0, u1, v0, v1;
    float p0, p1, q0, q1;
    pl_rect2df vis;

    if (!tex || dst_w <= 0 || dst_h <= 0 || src_w == 0 || src_h == 0) {
        return;
    }
    vis = (pl_rect2df){
        .x0 = FFMAX(dst->x0, 0.0f),
        .y0 = FFMAX(dst->y0, 0.0f),
        .x1 = FFMIN(dst->x1, (float)tex->params.w),
        .y1 = FFMIN(dst->y1, (float)tex->params.h),
    };
    if (vis.x1 <= vis.x0 || vis.y1 <= vis.y0) {
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

static pl_tex overlay_upload(RendererContext *ctx, pl_tex *slot, void *pixels,
                             int w, int h, int stride) {
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
        return NULL;
    }

    return *slot;
}

static void setup_render(RendererContext *ctx, struct pl_frame *pl_frame,
                         struct pl_frame *target, struct pl_render_params *pl_params,
                         RenderParams *params, struct pl_overlay *overlays,
                         struct pl_overlay_part *parts) {
    SDL_Rect *rect = &params->target_rect;
    target->crop = (pl_rect2df){.x0 = rect->x, .x1 = rect->x + rect->w, .y0 = rect->y, .y1 = rect->y + rect->h};

    pl_rotation rotation = pl_rotation_normalize(params->rotate / 90);
    if (ctx->sbs360_enabled && ctx->sbs360_hook) {
        pl_frame->rotation = PL_ROTATION_0;
    } else {
        pl_frame->rotation = rotation;
        clip_crops_to_target(pl_frame, target,
                             pl_rotation_normalize(rotation - target->rotation));
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

    if (ctx->sbs360_enabled && ctx->sbs360_hook) {
        view360_pl_hook_update(ctx->sbs360_hook, ctx->sbs360_yaw,
                               ctx->sbs360_pitch, ctx->sbs360_roll,
                               ctx->sbs360_hfov, ctx->sbs360_layout,
                               (int)rotation * 90);
        pl_params->hooks = &ctx->sbs360_hook;
        pl_params->num_hooks = 1;
    }

    int num_overlays = 0;

    if (params->sub_pixels && params->sub_width > 0 && params->sub_height > 0) {
        pl_tex tex = overlay_upload(ctx, &ctx->sub_tex, params->sub_pixels,
                                    params->sub_width, params->sub_height,
                                    params->sub_stride);
        if (tex) {
            parts[num_overlays] = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->sub_width, .y1 = (float)params->sub_height},
                .dst = {.x0 = rect->x, .y0 = rect->y, .x1 = rect->x + rect->w, .y1 = rect->y + rect->h},
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

    if (params->osd_pixels && params->osd_width > 0 && params->osd_height > 0) {
        pl_tex tex = overlay_upload(ctx, &ctx->osd_tex, params->osd_pixels,
                                    params->osd_width, params->osd_height,
                                    params->osd_stride);
        if (tex) {
            parts[num_overlays] = (struct pl_overlay_part){
                .src = {.x0 = 0, .y0 = 0, .x1 = (float)params->osd_width, .y1 = (float)params->osd_height},
                .dst = {.x0 = 0, .y0 = 0, .x1 = (float)params->osd_width, .y1 = (float)params->osd_height},
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

static void collect_present_timing(RendererContext *ctx, RenderParams *params) {
#if LACHESIS_HAVE_VULKAN
    VkPresentSample sample;

    if (ctx->api.backend != RENDERER_API_VULKAN) {
        return;
    }

    params->present_source = vkpresent_source();
    if (params->present_source == PRESENT_SOURCE_SWAP) {
        return;
    }

    if (vkpresent_poll(&sample)) {
        ctx->present_timing_silent = 0;
        params->present_source = sample.source;
        params->present_display_us = sample.display_us;
        params->present_refresh_us = sample.refresh_us;
        return;
    }

    if (++ctx->present_timing_silent > LACHESIS_PRESENT_TIMING_GRACE) {
        log_warn("No presentation feedback after %d presents. Timing the swap "
                 "on the CPU instead.\n",
                 LACHESIS_PRESENT_TIMING_GRACE);
        vkpresent_disable();
        params->present_source = PRESENT_SOURCE_SWAP;
    }
#else
    (void)ctx;
    (void)params;
#endif
}

static const struct pl_filter_config *pick_downscaler(const RendererContext *ctx,
                                                      const RenderParams *params) {
    if (ctx->benchmark || params->disable_linear_scaling || !params->still_image) {
        return &pl_filter_bilinear;
    }

    return &pl_filter_catmull_rom;
}

static struct pl_color_adjustment equalizer_adjustment(const RenderParams *params) {
    struct pl_color_adjustment adj = {PL_COLOR_ADJUSTMENT_NEUTRAL};

    adj.brightness = equalizer_pl_brightness(params->eq_brightness);
    adj.contrast = equalizer_pl_contrast(params->eq_contrast);
    adj.gamma = equalizer_pl_gamma(params->eq_gamma);

    return adj;
}

static int display(Renderer *renderer, AVFrame *frame, RenderParams *params) {
    struct pl_swapchain_frame swap_frame = {0};
    struct pl_frame pl_frame = {0};
    struct pl_frame target = {0};
    RendererContext *ctx = (RendererContext *)renderer;
    struct pl_color_adjustment color_adjustment = equalizer_adjustment(params);
    struct pl_render_params pl_params = {
        .upscaler = &pl_filter_bilinear,
        .downscaler = pick_downscaler(ctx, params),
        .sigmoid_params = ctx->benchmark ? NULL : pl_render_default_params.sigmoid_params,
        .color_adjustment = &color_adjustment,
        .dither_params = ctx->benchmark ? NULL : pl_render_default_params.dither_params,
        .cone_params = pl_render_default_params.cone_params,
        .color_map_params = pl_render_default_params.color_map_params,
        .disable_linear_scaling = ctx->benchmark || params->disable_linear_scaling,
        .skip_anti_aliasing = ctx->benchmark || params->skip_anti_aliasing,
    };
    int ret = 0;
    bool frame_started = false;
    struct pl_color_space hint = {0};
    int64_t _ts1, _ts2, _ts3 = 0, prs_us = 0;
    uint32_t max_dim;

    if (params->reset_history) {
        av_frame_free(&ctx->deint_prev);
    }

    ret = convert_frame(renderer, frame);
    if (ret < 0) {
        return ret;
    }

    if (params->next_frame && params->deinterlace == DEINTERLACE_YADIF) {
        convert_frame(renderer, params->next_frame);
    }

    if (frame->width <= 0 || frame->height <= 0) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    max_dim = ctx->gpu->limits.max_tex_2d_dim;
    if (max_dim && ((unsigned)frame->width > max_dim || (unsigned)frame->height > max_dim)) {
        ret = AVERROR(ERANGE);
        goto done;
    }

    if (!map_video_frame(ctx, frame, &pl_frame)) {
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    pl_color_space_from_avframe(&hint, frame);
    if (!ctx->have_hint || !pl_color_space_equal(&hint, &ctx->last_hint)) {
        pl_swapchain_colorspace_hint(ctx->swapchain, &hint);
        ctx->last_hint = hint;
        ctx->have_hint = true;
    }

    static int64_t t_acq, t_rnd, t_prs, t_n;
    int64_t _ts0 = av_gettime_relative();
    struct pl_frame pl_prev = {0}, pl_next = {0};
    bool mapped_prev = false, mapped_next = false;

    if (!pl_swapchain_start_frame(ctx->swapchain, &swap_frame)) {
        ret = AVERROR_EXTERNAL;
        goto out;
    }
    frame_started = true;
    _ts1 = av_gettime_relative();
    t_acq += _ts1 - _ts0;

    pl_frame_from_swapchain(&target, &swap_frame);

    if (ctx->icc_data) {
        target.profile = (struct pl_icc_profile){
            .data = ctx->icc_data,
            .len = ctx->icc_len,
            .signature = ctx->icc_sig,
        };
    }

    if (ctx->have_display_hdr) {
        pl_hdr_metadata_merge(&target.color.hdr, &ctx->display_hdr);
    }

    struct pl_overlay overlays[LACHESIS_MAX_OVERLAYS];
    struct pl_overlay_part parts[LACHESIS_MAX_OVERLAYS];

    setup_render(ctx, &pl_frame, &target, &pl_params, params, overlays, parts);
    apply_deinterlace(&pl_frame, &pl_params, frame, params);

    if (pl_params.deinterlace_params &&
        pl_deinterlace_needs_refs(pl_params.deinterlace_params->algo)) {
        pl_frame.prev = map_deint_ref(ctx, ctx->prev_tex, &pl_prev, &pl_frame,
                                      ctx->deint_prev, frame);
        pl_frame.next = map_deint_ref(ctx, ctx->next_tex, &pl_next, &pl_frame,
                                      params->next_frame, frame);
        mapped_prev = pl_frame.prev != NULL;
        mapped_next = pl_frame.next != NULL;
    }

    _ts2 = av_gettime_relative();
    if (!pl_render_image(ctx->renderer, &pl_frame, &target, &pl_params)) {
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
        double rnd_ms = (double)(_ts3 - _ts2) / 1000.0;
        double prs_ms = (double)prs_us / 1000.0;
        if (!ctx->stat_valid) {
            ctx->stat_acquire_ms = acq_ms;
            ctx->stat_render_ms = rnd_ms;
            ctx->stat_present_ms = prs_ms;
            ctx->stat_valid = 1;
        } else {
            const double ema_alpha = 1.0 / LACHESIS_STAT_EMA_FRAMES;
            ctx->stat_acquire_ms += (acq_ms - ctx->stat_acquire_ms) * ema_alpha;
            ctx->stat_render_ms += (rnd_ms - ctx->stat_render_ms) * ema_alpha;
            ctx->stat_present_ms += (prs_ms - ctx->stat_present_ms) * ema_alpha;
        }
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
    pl_unmap_avframe(ctx->gpu, &pl_frame);

done:
    if (params->deinterlace == DEINTERLACE_YADIF) {
        if (!ctx->deint_prev) {
            ctx->deint_prev = av_frame_alloc();
        }
        if (ctx->deint_prev && ret == 0) {
            av_frame_unref(ctx->deint_prev);
            av_frame_ref(ctx->deint_prev, frame);
        }
    } else if (ctx->deint_prev) {
        av_frame_free(&ctx->deint_prev);
    }

    return ret;
}

static int capture(Renderer *renderer, AVFrame *frame, RenderParams *params,
                   int width, int height, uint8_t *out, int out_stride) {
    RendererContext *ctx = (RendererContext *)renderer;
    struct pl_frame pl_frame = {0};
    struct pl_frame target = {0};
    struct pl_color_adjustment color_adjustment = equalizer_adjustment(params);
    struct pl_render_params pl_params = {
        .upscaler = &pl_filter_bilinear,
        .downscaler = pick_downscaler(ctx, params),
        .sigmoid_params = ctx->benchmark ? NULL : pl_render_default_params.sigmoid_params,
        .color_adjustment = &color_adjustment,
        .dither_params = ctx->benchmark ? NULL : pl_render_default_params.dither_params,
        .cone_params = pl_render_default_params.cone_params,
        .color_map_params = pl_render_default_params.color_map_params,
        .disable_linear_scaling = params->disable_linear_scaling,
        .skip_anti_aliasing = params->skip_anti_aliasing,
    };
    pl_tex cap_tex = NULL;
    struct pl_tex_params cap_params;
    struct pl_tex_transfer_params xfer;
    struct pl_frame pl_prev = {0}, pl_next = {0};
    bool mapped_prev = false, mapped_next = false;
    int ret = 0;

    ret = convert_frame(renderer, frame);
    if (ret < 0) {
        return ret;
    }

    if (params->next_frame && params->deinterlace == DEINTERLACE_YADIF) {
        convert_frame(renderer, params->next_frame);
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
    setup_render(ctx, &pl_frame, &target, &pl_params, params, overlays, parts);
    apply_deinterlace(&pl_frame, &pl_params, frame, params);

    if (pl_params.deinterlace_params &&
        pl_deinterlace_needs_refs(pl_params.deinterlace_params->algo)) {
        pl_frame.prev = map_deint_ref(ctx, ctx->prev_tex, &pl_prev, &pl_frame,
                                      ctx->deint_prev, frame);
        pl_frame.next = map_deint_ref(ctx, ctx->next_tex, &pl_next, &pl_frame,
                                      params->next_frame, frame);
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

    if (!ctx || !ctx->swapchain) {
        return AVERROR(EINVAL);
    }
    if (!pl_swapchain_resize(ctx->swapchain, &width, &height)) {
        return AVERROR_EXTERNAL;
    }
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
    av_freep(&ctx->pixfmts);
    ctx->num_pixfmts = 0;

    if (ctx->sbs360_hook) {
        view360_pl_hook_destroy(&ctx->sbs360_hook);
    }

    av_freep(&ctx->icc_data);

    if (ctx->gpu) {
#if LACHESIS_HAVE_PL_CACHE
        cache_save(ctx);
        pl_gpu_set_cache(ctx->gpu, NULL);
        pl_cache_destroy(&ctx->shader_cache);
        av_freep(&ctx->cache_path);
#endif
        pl_tex_destroy(ctx->gpu, &ctx->osd_tex);
        pl_tex_destroy(ctx->gpu, &ctx->sub_tex);
        av_frame_free(&ctx->deint_prev);
        for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->prev_tex); i++) {
            pl_tex_destroy(ctx->gpu, &ctx->prev_tex[i]);
            pl_tex_destroy(ctx->gpu, &ctx->next_tex[i]);
        }
        for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->tex); i++) {
            pl_tex_destroy(ctx->gpu, &ctx->tex[i]);
        }
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

static const AVClass renderer_class = {
    .class_name = "Renderer",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

static const enum RendererApi renderer_api_order[] = {
#if LACHESIS_HAVE_VULKAN
    RENDERER_API_VULKAN,
#endif
#if LACHESIS_HAVE_D3D11
    RENDERER_API_D3D11,
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

    ret = create(renderer, window, params->opt);
    if (ret < 0) {
        note_failure(why, why_size, what, SDL_GetError(), ret);
        goto fail;
    }

    const char *video_driver = SDL_GetCurrentVideoDriver();
    int show_before_test = video_driver && !strcmp(video_driver, "wayland");

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

    *out_window = window;
    *out_renderer = renderer;

    return 0;

fail:
    destroy(renderer);
    av_free(renderer);
    SDL_DestroyWindow(window);

    return ret;
}

int renderer_open(const RendererOpenParams *params, SDL_Window **window,
                  Renderer **out, char *why, size_t why_size) {
    enum RendererApi order[FF_ARRAY_ELEMS(renderer_api_order)];
    const char *driver = SDL_GetCurrentVideoDriver();
    size_t num = 0;
    int last = AVERROR(ENOSYS);

    why[0] = '\0';

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

    for (size_t i = 0; i < num; i++) {
        enum RendererApi api = order[i];
        int attempts = api_num_attempts(api, params->opt);

        for (int attempt = 0; attempt < attempts; attempt++) {
            int ret = renderer_try(params, api, attempt, window, out, why,
                                   why_size);

            if (ret >= 0) {
                return 0;
            }
            last = ret;
        }
        if (i + 1 < num) {
            log_verbose("The %s renderer is unavailable. Trying %s.\n",
                        api_label(api), api_label(order[i + 1]));
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

    if (!ctx) {
        return 0;
    }

    return icc_load_display(ctx, window) | hdr_refresh(ctx, window);
}

int renderer_enable_360(Renderer *renderer, enum View360Layout layout) {
    RendererContext *ctx = (RendererContext *)renderer;
    int enable = layout != VIEW360_LAYOUT_OFF;

    if (!ctx) {
        return AVERROR(EINVAL);
    }
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

    return 0;
}

void renderer_update_360(Renderer *renderer, float yaw, float pitch, float roll, float hfov) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return;
    }
    ctx->sbs360_yaw = yaw;
    ctx->sbs360_pitch = pitch;
    ctx->sbs360_roll = roll;
    ctx->sbs360_hfov = hfov;
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
    return display(renderer, frame, render_params);
}

int renderer_display_blank(Renderer *renderer, RenderParams *render_params) {
    return display_blank(renderer, render_params);
}

int renderer_capture(Renderer *renderer, AVFrame *frame, RenderParams *render_params,
                     int width, int height, uint8_t *out, int out_stride) {
    return capture(renderer, frame, render_params, width, height, out, out_stride);
}

int renderer_resize(Renderer *renderer, int width, int height) {
    return resize(renderer, width, height);
}

void renderer_destroy(Renderer *renderer) {
    if (!renderer) {
        return;
    }
    destroy(renderer);
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
    case RENDERER_API_OPENGL: {
        int interval = 0;
        if (!SDL_GL_GetSwapInterval(&interval)) {
            return 1;
        }
        return interval > 0;
    }
#endif
    default:
        return 1;
    }
}

int renderer_frame_stats(Renderer *renderer, double *acquire_ms,
                         double *render_ms, double *present_ms) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx || !ctx->stat_valid) {
        return 0;
    }
    if (acquire_ms) {
        *acquire_ms = ctx->stat_acquire_ms;
    }
    if (render_ms) {
        *render_ms = ctx->stat_render_ms;
    }
    if (present_ms) {
        *present_ms = ctx->stat_present_ms;
    }

    return 1;
}
