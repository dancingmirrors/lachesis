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
#include "lachesis_hwaccel.h"
#include "lachesis_log.h"
#include "lachesis_renderer_internal.h"
/* clang-format on */

#if LACHESIS_HAVE_VULKAN

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>

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

/* https://github.com/KhronosGroup/MoltenVK/issues/2618 */
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

static int list_vk_devices(PFN_vkGetInstanceProcAddr get_proc_addr,
                           VkInstance inst, GpuDeviceNames names,
                           enum GpuClass *classes) {
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties get_props;
    VkPhysicalDevice devices[MAX_GPU_DEVICES];
    uint32_t num = MAX_GPU_DEVICES;

    enumerate = (PFN_vkEnumeratePhysicalDevices)
        get_proc_addr(inst, "vkEnumeratePhysicalDevices");
    get_props = (PFN_vkGetPhysicalDeviceProperties)
        get_proc_addr(inst, "vkGetPhysicalDeviceProperties");
    if (!enumerate || !get_props) {
        return 0;
    }
    if (enumerate(inst, &num, devices) < 0) {
        return 0;
    }
    if (num > MAX_GPU_DEVICES) {
        num = MAX_GPU_DEVICES;
    }

    for (uint32_t i = 0; i < num; i++) {
        VkPhysicalDeviceProperties props;

        get_props(devices[i], &props);
        snprintf(names[i], 256, "%s", props.deviceName);
        if (!classes) {
            continue;
        }
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            classes[i] = GPU_CLASS_DISCRETE;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            classes[i] = GPU_CLASS_INTEGRATED;
            break;
        default:
            classes[i] = GPU_CLASS_ANY;
            break;
        }
    }

    return (int)num;
}

int list_vk_devices_standalone(GpuDeviceNames names,
                               enum GpuClass *classes) {
    PFN_vkGetInstanceProcAddr get_proc_addr;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    };
    VkInstance inst = VK_NULL_HANDLE;
    int had_video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    int num;

    if (!had_video && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        log_dead("No video subsystem to list Vulkan devices with: %s\n",
                 SDL_GetError());
        return AVERROR_EXTERNAL;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        log_dead("Vulkan is not available: %s\n", SDL_GetError());
        if (!had_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        return AVERROR_EXTERNAL;
    }

    get_proc_addr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    create_instance = get_proc_addr
        ? (PFN_vkCreateInstance)get_proc_addr(NULL, "vkCreateInstance")
        : NULL;
#ifdef VK_KHR_portability_enumeration
    /* MoltenVK and friends are hidden from a plain instance. */
    {
        static const char *const portability[] = {
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        };
        VkInstanceCreateInfo portable = info;

        portable.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        portable.enabledExtensionCount = 1;
        portable.ppEnabledExtensionNames = portability;
        if (create_instance &&
            create_instance(&portable, NULL, &inst) == VK_SUCCESS) {
            create_instance = NULL;
        }
    }
#endif

    if (inst == VK_NULL_HANDLE &&
        (!create_instance ||
         create_instance(&info, NULL, &inst) != VK_SUCCESS)) {
        SDL_Vulkan_UnloadLibrary();
        if (!had_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        log_dead("Failed to create a Vulkan instance to list devices.\n");
        return AVERROR_EXTERNAL;
    }

    num = list_vk_devices(get_proc_addr, inst, names, classes);

    destroy_instance =
        (PFN_vkDestroyInstance)get_proc_addr(inst, "vkDestroyInstance");
    if (destroy_instance) {
        destroy_instance(inst, NULL);
    }
    SDL_Vulkan_UnloadLibrary();
    if (!had_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    return num;
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

static void note_decode_caps(RendererContext *ctx, const char *const *exts,
                             int num_exts) {
    ctx->decode_caps = 0;

    for (int i = 0; i < num_exts; i++) {
        if (!strcmp(exts[i], "VK_KHR_video_decode_h264")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_H264;
        } else if (!strcmp(exts[i], "VK_KHR_video_decode_h265")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_HEVC;
        } else if (!strcmp(exts[i], "VK_KHR_video_decode_av1")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_AV1;
        } else if (!strcmp(exts[i], "VK_KHR_video_decode_vp9")) {
            ctx->decode_caps |= RENDERER_DECODE_CAP_VP9;
        }
    }
}

typedef struct VkFeatureLocation {
    VkStructureType type;
    size_t offset;
} VkFeatureLocation;

typedef struct VkBackedExtension {
    const char *name;
    const char *feature;
    VkFeatureLocation at[2];
} VkBackedExtension;

/* clang-format off */
static const VkBackedExtension backed_extensions[] = {
    {VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME, "shaderSubgroupRotate",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES_KHR,
       offsetof(VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR, shaderSubgroupRotate)},
#ifdef VK_VERSION_1_4
      {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
       offsetof(VkPhysicalDeviceVulkan14Features, shaderSubgroupRotate)},
#endif
     }},
    {VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, "hostImageCopy",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT,
       offsetof(VkPhysicalDeviceHostImageCopyFeaturesEXT, hostImageCopy)},
#ifdef VK_VERSION_1_4
      {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
       offsetof(VkPhysicalDeviceVulkan14Features, hostImageCopy)},
#endif
     }},
    {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, "shaderObject",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
       offsetof(VkPhysicalDeviceShaderObjectFeaturesEXT, shaderObject)},
     }},
    {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, "cooperativeMatrix",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
       offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, cooperativeMatrix)},
     }},
    {VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, "shaderBufferFloat32Atomics",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,
       offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, shaderBufferFloat32Atomics)},
     }},
    {VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME, "workgroupMemoryExplicitLayout",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR,
       offsetof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout)},
     }},
    {VK_NV_OPTICAL_FLOW_EXTENSION_NAME, "opticalFlow",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV,
       offsetof(VkPhysicalDeviceOpticalFlowFeaturesNV, opticalFlow)},
     }},
    {VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME, "videoMaintenance1",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR,
       offsetof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR, videoMaintenance1)},
     }},
#ifdef VK_KHR_video_maintenance2
    {VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME, "videoMaintenance2",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR,
       offsetof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR, videoMaintenance2)},
     }},
#endif
    /* Due to the way libplacebo's vk_features_normalize() works, listing
     * VK_KHR_video_decode_vp9 here would only cause us to withhold it.
     */
#ifdef VK_KHR_video_encode_av1
    {VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, "videoEncodeAV1",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR,
       offsetof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR, videoEncodeAV1)},
     }},
#endif
#ifdef VK_EXT_shader_long_vector
    {VK_EXT_SHADER_LONG_VECTOR_EXTENSION_NAME, "longVector",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_LONG_VECTOR_FEATURES_EXT,
       offsetof(VkPhysicalDeviceShaderLongVectorFeaturesEXT, longVector)},
     }},
#endif
#ifdef VK_EXT_shader_replicated_composites
    {VK_EXT_SHADER_REPLICATED_COMPOSITES_EXTENSION_NAME, "shaderReplicatedComposites",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT,
       offsetof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT, shaderReplicatedComposites)},
     }},
#endif
#ifdef VK_EXT_zero_initialize_device_memory
    {VK_EXT_ZERO_INITIALIZE_DEVICE_MEMORY_EXTENSION_NAME, "zeroInitializeDeviceMemory",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_DEVICE_MEMORY_FEATURES_EXT,
       offsetof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT, zeroInitializeDeviceMemory)},
     }},
#endif
#ifdef VK_KHR_shader_expect_assume
    {VK_KHR_SHADER_EXPECT_ASSUME_EXTENSION_NAME, "shaderExpectAssume",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES_KHR,
       offsetof(VkPhysicalDeviceShaderExpectAssumeFeaturesKHR, shaderExpectAssume)},
#ifdef VK_VERSION_1_4
      {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
       offsetof(VkPhysicalDeviceVulkan14Features, shaderExpectAssume)},
#endif
     }},
#endif
#ifdef VK_KHR_shader_maximal_reconvergence
    {VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME, "shaderMaximalReconvergence",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
       offsetof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR, shaderMaximalReconvergence)},
     }},
#endif
#ifdef VK_KHR_shader_relaxed_extended_instruction
    {VK_KHR_SHADER_RELAXED_EXTENDED_INSTRUCTION_EXTENSION_NAME, "shaderRelaxedExtendedInstruction",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR,
       offsetof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR, shaderRelaxedExtendedInstruction)},
     }},
#endif
#ifdef VK_KHR_maintenance9
    {VK_KHR_MAINTENANCE_9_EXTENSION_NAME, "maintenance9",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,
       offsetof(VkPhysicalDeviceMaintenance9FeaturesKHR, maintenance9)},
     }},
#endif
#ifdef VK_KHR_unified_image_layouts
    {VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, "unifiedImageLayouts",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
       offsetof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR, unifiedImageLayouts)},
     }},
#endif
#ifdef VK_KHR_internally_synchronized_queues
    {VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME, "internallySynchronizedQueues",
     {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR,
       offsetof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR, internallySynchronizedQueues)},
     }},
#endif
};
/* clang-format on */

static const void *find_feature(const VkPhysicalDeviceFeatures2 *features,
                                VkStructureType type) {
    const VkBaseInStructure *entry = features ? features->pNext : NULL;

    for (; entry; entry = entry->pNext) {
        if (entry->sType == type) {
            return entry;
        }
    }

    return NULL;
}

static const VkBackedExtension *
unbacked_extension(const char *name,
                   const VkPhysicalDeviceFeatures2 *features) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(backed_extensions); i++) {
        const VkBackedExtension *backed = &backed_extensions[i];

        if (strcmp(name, backed->name)) {
            continue;
        }
        for (size_t n = 0; n < FF_ARRAY_ELEMS(backed->at); n++) {
            const VkFeatureLocation *at = &backed->at[n];
            const char *entry;
            VkBool32 enabled;

            if (!at->type) {
                break;
            }
            entry = find_feature(features, at->type);
            if (!entry) {
                continue;
            }
            memcpy(&enabled, entry + at->offset, sizeof(enabled));
            if (enabled) {
                return NULL;
            }
        }

        return backed;
    }

    return NULL;
}

static int drop_unbacked_extensions(RendererContext *ctx,
                                    const char *const *exts, int num_exts,
                                    const VkPhysicalDeviceFeatures2 *features,
                                    const char *const **out_exts,
                                    int *out_num) {
    const char **kept;
    int n = 0;

    *out_exts = exts;
    *out_num = num_exts;
    for (int i = 0; i < num_exts; i++) {
        if (unbacked_extension(exts[i], features)) {
            n = 1;
            break;
        }
    }
    if (!n) {
        return 0;
    }

    kept = av_calloc(num_exts, sizeof(*kept));
    if (!kept) {
        return AVERROR(ENOMEM);
    }
    n = 0;
    for (int i = 0; i < num_exts; i++) {
        const VkBackedExtension *backed = unbacked_extension(exts[i], features);

        if (backed) {
            log_verbose("Withholding %s: the device has no %s.\n",
                        backed->name, backed->feature);
            continue;
        }
        kept[n++] = exts[i];
    }

    av_free(ctx->unbacked_dev_exts);
    ctx->unbacked_dev_exts = kept;
    *out_exts = (const char *const *)kept;
    *out_num = n;

    return 0;
}

static int internally_synchronized(const VkPhysicalDeviceFeatures2 *features) {
#ifdef VK_KHR_internally_synchronized_queues
    const VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR *isq =
        find_feature(
            features,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR);

    return isq && isq->internallySynchronizedQueues;
#else
    (void)features;
    return 0;
#endif
}

static uint32_t nvidia_proprietary(PFN_vkGetInstanceProcAddr get_proc_addr,
                                   VkInstance inst, VkPhysicalDevice phys) {
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver,
    };

    if (!get_proc_addr || !inst || !phys) {
        return 0;
    }
    get_props2 = (PFN_vkGetPhysicalDeviceProperties2)
        get_proc_addr(inst, "vkGetPhysicalDeviceProperties2");
    if (!get_props2) {
        return 0;
    }
    get_props2(phys, &props);

    if (driver.driverID != VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        return 0;
    }

    return props.properties.driverVersion;
}

#ifdef LACHESIS_HAVE_VK_DRM_NODE

int vk_render_node(PFN_vkGetInstanceProcAddr get_proc_addr,
                   VkInstance inst, VkPhysicalDevice phys, char *buf,
                   size_t size) {
    PFN_vkEnumerateDeviceExtensionProperties enumerate;
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    VkPhysicalDeviceDrmPropertiesEXT drm = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm,
    };
    VkExtensionProperties *exts;
    uint32_t num = 0;
    int supported = 0;
    struct stat st;

    if (!buf || !size) {
        return AVERROR(EINVAL);
    }
    buf[0] = '\0';

    if (!get_proc_addr || !inst || !phys) {
        return AVERROR(ENOSYS);
    }
    enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
        get_proc_addr(inst, "vkEnumerateDeviceExtensionProperties");
    get_props2 = (PFN_vkGetPhysicalDeviceProperties2)
        get_proc_addr(inst, "vkGetPhysicalDeviceProperties2");
    if (!enumerate || !get_props2) {
        return AVERROR(ENOSYS);
    }

    if (enumerate(phys, NULL, &num, NULL) != VK_SUCCESS || !num) {
        return AVERROR(ENOSYS);
    }
    exts = av_calloc(num, sizeof(*exts));
    if (!exts) {
        return AVERROR(ENOMEM);
    }
    if (enumerate(phys, NULL, &num, exts) == VK_SUCCESS) {
        for (uint32_t i = 0; i < num && !supported; i++) {
            supported = !strcmp(exts[i].extensionName,
                                VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        }
    }
    av_free(exts);
    if (!supported) {
        return AVERROR(ENOSYS);
    }

    get_props2(phys, &props);
    if (!drm.hasRender) {
        return AVERROR(ENOSYS);
    }

    snprintf(buf, size, "/dev/dri/renderD%" PRId64, drm.renderMinor);
    if (stat(buf, &st) < 0 || !S_ISCHR(st.st_mode) ||
        (int64_t)major(st.st_rdev) != drm.renderMajor ||
        (int64_t)minor(st.st_rdev) != drm.renderMinor) {
        buf[0] = '\0';
        return AVERROR(ENOENT);
    }

    return 0;
}

#endif /* LACHESIS_HAVE_VK_DRM_NODE */

static int create_vk_by_hwcontext(Renderer *renderer,
                                  const char **ext, unsigned num_ext,
                                  const AVDictionary *opt, int present_timing) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *dev;
    AVVulkanDeviceContext *hwctx;
    AVDictionary *dict = NULL;
    const char *raw_device;
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

    raw_device = select_device(opt);
    if (!raw_device && renderer_want_device) {
        GpuDeviceNames names;
        enum GpuClass classes[MAX_GPU_DEVICES];
        int num = list_vk_devices_standalone(names, classes);
        int match = num > 0
            ? renderer_match_gpu_device(names, classes, num, renderer_want_device)
            : -1;

        if (match >= 0) {
            av_strlcpy(ctx->device_request, names[match],
                       sizeof(ctx->device_request));
            raw_device = ctx->device_request;
        } else {
            log_warn("No Vulkan device matches '%s'.\n",
                     renderer_want_device);
            if (num > 0) {
                renderer_report_gpu_devices("Vulkan", names, num, 0);
            }
        }
    }
    ret = av_hwdevice_ctx_create(&ctx->hw_device_ref, AV_HWDEVICE_TYPE_VULKAN,
                                 raw_device, dict, 0);
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
        .get_proc_addr = vkpresent_wrap_proc_addr(hwctx->get_proc_addr),
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

#if defined(VK_KHR_internally_synchronized_queues) && \
    LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
    if (hwctx->queue_flags &
        VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR) {
#if PL_API_VER >= 365
        import_params.queue_graphics.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
        import_params.queue_compute.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
        import_params.queue_transfer.flags |= VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
        import_params.lock_queue = NULL;
        import_params.unlock_queue = NULL;
#else
        log_warn("VK_KHR_internally_synchronized_queues with libplacebo < 365 hack.\n");
        av_buffer_unref(&ctx->hw_device_ref);
        ctx->inst = NULL;
        return create_vk_by_placebo(renderer, ext, num_ext, opt,
                                    present_timing);
#endif
    }
#endif
    ctx->dev_extensions = import_exts;
    ctx->num_dev_extensions = num_import_exts;
    ctx->dev_features = &hwctx->device_features;

    ctx->placebo_vulkan = pl_vulkan_import(ctx->log_ctx, &import_params);
    if (!ctx->placebo_vulkan) {
        return AVERROR_EXTERNAL;
    }
    note_decode_caps(ctx, import_exts, num_import_exts);

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

#define MAX_DECODE_FAMILIES 8

static int get_decode_queues(Renderer *renderer, uint32_t *index,
                             uint32_t *count) {
    RendererContext *ctx = (RendererContext *)renderer;
    VkQueueFamilyProperties *queue_family_prop = NULL;
    uint32_t num_queue_family_prop = 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_prop;
    PFN_vkGetInstanceProcAddr get_proc_addr = ctx->get_proc_addr;
    int num = 0;

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

    for (uint32_t i = 0; i < num_queue_family_prop; i++) {
        if (!(queue_family_prop[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) {
            continue;
        }
        if (num >= MAX_DECODE_FAMILIES) {
            log_verbose("Ignoring video decode queue families past %d.\n",
                        MAX_DECODE_FAMILIES);
            break;
        }
        index[num] = i;
        count[num] = queue_family_prop[i].queueCount;
        num++;
    }
    av_free(queue_family_prop);

    return num;
}

static void add_queue_family(AVVulkanDeviceContext *hwctx, int *nb_qf,
                             uint32_t idx, uint32_t num,
                             VkQueueFlagBits flags) {
    if (!num) {
        return;
    }
    for (int i = 0; i < *nb_qf; i++) {
        if (hwctx->qf[i].idx == (int)idx) {
            hwctx->qf[i].flags |= flags;
            return;
        }
    }
    if (*nb_qf >= (int)FF_ARRAY_ELEMS(hwctx->qf)) {
        return;
    }

    hwctx->qf[*nb_qf] = (AVVulkanDeviceQueueFamily){
        .idx = idx,
        .num = num,
        .flags = flags,
    };
    (*nb_qf)++;
}

static int create_vk_by_placebo(Renderer *renderer,
                                const char **ext, unsigned num_ext,
                                const AVDictionary *opt, int present_timing) {
    RendererContext *ctx = (RendererContext *)renderer;
    AVHWDeviceContext *device_ctx;
    AVVulkanDeviceContext *vk_dev_ctx;
    PFN_vkGetInstanceProcAddr placebo_proc_addr;
    const char *device_name;
    const char **opt_exts = NULL;
    const char **merged_exts = NULL;
    int num_opt_exts = 0;
    uint32_t decode_index[MAX_DECODE_FAMILIES];
    uint32_t decode_count[MAX_DECODE_FAMILIES];
    int num_decode;
    int ret;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    const char **dev_exts;
    int num_dev_exts;
#endif

    ctx->get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    placebo_proc_addr = vkpresent_wrap_proc_addr(ctx->get_proc_addr);
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

    {
        GpuDeviceNames names;
        enum GpuClass classes[MAX_GPU_DEVICES];
        int num = list_vk_devices(ctx->get_proc_addr,
                                  ctx->placebo_instance->instance, names,
                                  classes);

        renderer_report_gpu_devices("Vulkan", names, num, 1);

        device_name = select_device(opt);
        if (!device_name && renderer_want_device) {
            int match = renderer_match_gpu_device(names, classes, num, renderer_want_device);

            if (match < 0) {
                log_warn("No Vulkan device matches '%s'.\n",
                         renderer_want_device);
                renderer_report_gpu_devices("Vulkan", names, num, 0);
            } else {
                av_strlcpy(ctx->device_request, names[match],
                           sizeof(ctx->device_request));
                device_name = ctx->device_request;
            }
        }
    }

    /* clang-format off */
    ctx->placebo_vulkan = pl_vulkan_create(ctx->log_ctx,
                                           pl_vulkan_params(
                                               .instance = ctx->placebo_instance->instance,
                                               .get_proc_addr = ctx->placebo_instance->get_proc_addr,
                                               .surface = ctx->vk_surface,
                                               .allow_software = renderer_allow_software_gpu,
                                               .opt_extensions = opt_exts,
                                               .num_opt_extensions = num_opt_exts,
                                               .features = present_timing ? vkpresent_device_features() : NULL,
                                               .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                                               .device_name = device_name, ));
    /* clang-format on */
    av_free(merged_exts);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 20, 100)
    av_free(dev_exts);
#endif
    if (!ctx->placebo_vulkan) {
        return AVERROR_EXTERNAL;
    }
    ctx->dev_features = ctx->placebo_vulkan->features;
    ret = drop_unbacked_extensions(ctx, ctx->placebo_vulkan->extensions,
                                   ctx->placebo_vulkan->num_extensions,
                                   ctx->dev_features, &ctx->dev_extensions,
                                   &ctx->num_dev_extensions);
    if (ret < 0) {
        return ret;
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
    if (!internally_synchronized(ctx->dev_features)) {
        vk_dev_ctx->lock_queue = placebo_lock_queue;
        vk_dev_ctx->unlock_queue = placebo_unlock_queue;
    }
    FF_ENABLE_DEPRECATION_WARNINGS
#endif

    vk_dev_ctx->get_proc_addr = ctx->get_proc_addr;

    vk_dev_ctx->inst = ctx->placebo_instance->instance;
    vk_dev_ctx->phys_dev = ctx->placebo_vulkan->phys_device;
    vk_dev_ctx->act_dev = ctx->placebo_vulkan->device;

    vk_dev_ctx->device_features = *ctx->placebo_vulkan->features;

    vk_dev_ctx->enabled_inst_extensions = ctx->placebo_instance->extensions;
    vk_dev_ctx->nb_enabled_inst_extensions = ctx->placebo_instance->num_extensions;

    vk_dev_ctx->enabled_dev_extensions = ctx->dev_extensions;
    vk_dev_ctx->nb_enabled_dev_extensions = ctx->num_dev_extensions;

    /* Otherwise we get 16 graphics queues. */
    uint32_t nvidia = nvidia_proprietary(ctx->get_proc_addr, ctx->inst,
                                         ctx->placebo_vulkan->phys_device);
    int nb_qf = 0;
    add_queue_family(vk_dev_ctx, &nb_qf,
                     ctx->placebo_vulkan->queue_graphics.index,
                     nvidia ? FFMIN(ctx->placebo_vulkan->queue_graphics.count, 1)
                            : ctx->placebo_vulkan->queue_graphics.count,
                     VK_QUEUE_GRAPHICS_BIT);
    add_queue_family(vk_dev_ctx, &nb_qf,
                     ctx->placebo_vulkan->queue_transfer.index,
                     ctx->placebo_vulkan->queue_transfer.count,
                     VK_QUEUE_TRANSFER_BIT);
    add_queue_family(vk_dev_ctx, &nb_qf,
                     ctx->placebo_vulkan->queue_compute.index,
                     ctx->placebo_vulkan->queue_compute.count,
                     VK_QUEUE_COMPUTE_BIT);

    num_decode = get_decode_queues(renderer, decode_index, decode_count);
    if (num_decode < 0) {
        return num_decode;
    }
    for (int i = 0; i < num_decode; i++) {
        add_queue_family(vk_dev_ctx, &nb_qf, decode_index[i], decode_count[i],
                         VK_QUEUE_VIDEO_DECODE_BIT_KHR);
    }
    vk_dev_ctx->nb_qf = nb_qf;

    ret = av_hwdevice_ctx_init(ctx->hw_device_ref);
    if (ret < 0) {
        return ret;
    }

    note_decode_caps(ctx, ctx->dev_extensions, ctx->num_dev_extensions);

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

static int surface_allows_opaque(RendererContext *ctx) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_caps;
    VkSurfaceCapabilitiesKHR caps;

    get_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
                   ctx->get_proc_addr(ctx->inst,
                                      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!get_caps) {
        return 0;
    }
    if (get_caps(ctx->placebo_vulkan->phys_device, ctx->vk_surface, &caps) !=
        VK_SUCCESS) {
        return 0;
    }

    return !!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
}

int vk_backend_create(RendererContext *ctx, SDL_Window *window,
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

    vkpresent_force_opaque(!renderer_want_translucent && surface_allows_opaque(ctx));

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

void vk_backend_destroy(RendererContext *ctx) {
    vkpresent_disable();

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
    av_freep(&ctx->unbacked_dev_exts);

    vkpresent_shutdown();
}

#endif /* LACHESIS_HAVE_VULKAN */
