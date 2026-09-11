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

#ifndef LACHESIS_RENDERER_INTERNAL_H
#define LACHESIS_RENDERER_INTERNAL_H

/* clang-format off */
#include "lachesis_cache.h"
#include "lachesis_config.h"
#include "lachesis_hwaccel.h"
#include "lachesis_icc.h"
#include "lachesis_renderer.h"
#include "lachesis_supersample.h"
#include "lachesis_view360.h"
/* clang-format on */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>

#include <libplacebo/config.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/utils/libav.h>

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
#include <dxgi1_6.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libplacebo/d3d11.h>

#include "lachesis_present_d3d11.h"
#endif

#ifdef LACHESIS_HAVE_DRM_NODES
#include <dirent.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#endif

#if LACHESIS_HAVE_VULKAN && defined(LACHESIS_HAVE_DRM_NODES) && \
    defined(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)
#define LACHESIS_HAVE_VK_DRM_NODE 1
#endif

#define LACHESIS_D3D11_VIEW_POOLS 6

struct Renderer {
    const AVClass *class;

    enum RendererApi backend;
};

typedef struct ImageState {
    SDL_Rect rect;
    int rotate;
    int changed;
    int moving;
} ImageState;

typedef struct ImageTracker {
    ImageState last;
    int64_t changed_at;
    int64_t repaint_failed_at;
    int seen;
    int repaint_asked;
} ImageTracker;

typedef struct VoOverlay {
    uint8_t *pixels;
    size_t size;
    unsigned generation;
} VoOverlay;

typedef struct VoFeedback {
    int64_t done_us;
    int64_t block_us;
    int source;
    int64_t display_us;
    double refresh_us;
} VoFeedback;

#define VO_FEEDBACK_RING 8

/* State the event loop wanted set but could not. */
#define VO_PENDING_360 (1u << 0)
#define VO_PENDING_SUPERSAMPLE (1u << 1)

typedef struct VoFrame {
    AVFrame *frame;
    uint64_t id;
} VoFrame;

typedef struct Vo {
    SDL_Thread *thread;
    SDL_Mutex *lock;
    /* The thread waits here for something to draw. */
    SDL_Condition *wake;
    /* Everyone else waits here for it to finish. */
    SDL_Condition *idle;

    int quit;
    int busy;
    int borrowed;
    int have_job;
    int blank;
    int abandoned;
    int have_status;
    int last_status;

    RenderParams params;
    VoFrame frame;
    VoFrame prev_frame;
    VoFrame next_frame;
    VoFrame mix_frame[LACHESIS_MAX_MIX_FRAMES];
    RenderMixFrame mix[LACHESIS_MAX_MIX_FRAMES];
    VoOverlay osd;
    VoOverlay sub;
    VoOverlay text_sub;

    float view360_yaw, view360_pitch, view360_roll, view360_hfov;

    unsigned pending;
    enum View360Layout pending_360_layout;
    enum View360Projection pending_360_projection;
    enum SupersampleLevel pending_supersample;

    VoFeedback feedback[VO_FEEDBACK_RING];
    unsigned feedback_head;
    unsigned feedback_tail;
    unsigned feedback_epoch;
} Vo;

typedef struct RendererContext {
    Renderer api;

    Vo vo;
    SDL_Window *window;
    int gl_swap_interval;

    pl_gpu gpu;
    pl_swapchain swapchain;
    pl_renderer renderer;
    pl_log log_ctx;

    pl_tex tex[4];
    pl_tex prev_tex[4];
    pl_tex next_tex[4];
    AVFrame *sw_frame;

    HwDownload readback;

    struct MixSlot {
        uint64_t signature;
        int mapped;
        int used;
        pl_tex tex[4];
        struct pl_frame frame;
    } mix_slots[LACHESIS_MAX_MIX_FRAMES];

    AVBufferRef *hw_device_ref;

#if LACHESIS_HAVE_VULKAN
    pl_vk_inst placebo_instance;
    pl_vulkan placebo_vulkan;
    VkSurfaceKHR vk_surface;

    AVBufferRef *hw_frame_ref;
    enum AVPixelFormat *transfer_formats;
    AVHWFramesConstraints *constraints;
    char device_request[256];
    unsigned decode_caps;
    /* Not necessarily the requested mode. */
    VkPresentModeKHR present_mode;

    PFN_vkGetInstanceProcAddr get_proc_addr;
    VkInstance inst;

    const char *const *dev_extensions;
    int num_dev_extensions;
    const VkPhysicalDeviceFeatures2 *dev_features;

    const char **filtered_dev_exts;
    const char **unbacked_dev_exts;

    AVFrame *vk_frame;
#endif

#if LACHESIS_HAVE_OPENGL
    pl_opengl placebo_gl;
    SDL_GLContext gl_context;
    SDL_EGLDisplay gl_egl_display;
    SDL_ThreadID gl_pinned_by;
    char gl_drm_node[64];
#endif

#if LACHESIS_HAVE_D3D11
    pl_d3d11 placebo_d3d11;
    ID3D10Multithread *d3d11_multithread;
    struct D3D11ViewPool {
        ID3D11Texture2D *texture;
        pl_tex *views;
        unsigned num_views;
        uint64_t serial;
    } d3d11_pools[LACHESIS_D3D11_VIEW_POOLS];
    uint64_t d3d11_serial;
#endif

    /* See build_pixfmt_list(). */
    enum AVPixelFormat *pixfmts;
    int num_pixfmts;

    int present_timing_silent;

    int swapchain_stale;
    int swapchain_stale_w;
    int swapchain_stale_h;
    int swapchain_retry;

    int zero_copy_failed;
    int zero_copy_misses;
    struct ZeroCopyPool {
        enum AVPixelFormat sw_format;
        int width;
        int height;
    } zero_copy_pool, zero_copy_failed_pool;

    char api_name[64];
    char device_name[256];

    const struct pl_hook *sbs360_hook;
    int sbs360_enabled;
    float sbs360_yaw;
    float sbs360_pitch;
    float sbs360_roll;
    float sbs360_hfov;
    enum View360Layout sbs360_layout;
    enum View360Projection sbs360_projection;

    const struct pl_hook *supersample_hook;
    enum SupersampleLevel supersample_level;

    int benchmark;

    double stat_acquire_ms;
    double stat_convert_ms;
    double stat_render_ms;
    double stat_present_ms;
    int stat_valid;

    ImageTracker image;

    struct pl_color_space last_hint;
    bool have_hint;

    pl_tex osd_tex;
    pl_tex sub_tex;
    pl_tex text_sub_tex;
    unsigned osd_tex_generation;
    unsigned sub_tex_generation;
    unsigned text_sub_tex_generation;

    AVFrame *blank_frame;

    int quiesced;
    int gpu_busy;

    ShaderCache shader_cache;

    void *icc_data;
    size_t icc_len;
    uint64_t icc_sig;
    int icc_from_file;
    int icc_auto;
    int icc_vcgt;
    int icc_vcgt_loaded;
    enum pl_rendering_intent icc_intent;
    pl_icc_object icc_obj;

    double white_point;
    IccGammaRamp cal_ramp;
    struct pl_custom_lut cal_lut;

    int hdr_auto;
    int hdr_warned;
    int have_display_hdr;
    struct pl_hdr_metadata display_hdr;
} RendererContext;

extern int renderer_allow_software_gpu;
extern int renderer_want_translucent;
extern const char *renderer_want_device;

static inline int enable_debug(const AVDictionary *opt) {
    AVDictionaryEntry *entry = av_dict_get(opt, "debug", NULL, 0);
    int debug = entry && strtol(entry->value, NULL, 10);
    return debug;
}

#define MAX_GPU_DEVICES 16

enum GpuClass {
    GPU_CLASS_ANY,
    GPU_CLASS_INTEGRATED,
    GPU_CLASS_DISCRETE,
};

typedef char GpuDeviceNames[MAX_GPU_DEVICES][256];

enum GpuClass renderer_gpu_class_request(const char *want);
void renderer_report_gpu_devices(const char *api, const GpuDeviceNames names,
                                 int num, int verbose);
int renderer_match_gpu_device(const GpuDeviceNames names,
                              const enum GpuClass *classes, int num,
                              const char *want);

void icc_setup(RendererContext *ctx, SDL_Window *window,
               const AVDictionary *opt);
void icc_forget(RendererContext *ctx);
void icc_track_luma(RendererContext *ctx, float max_luma);
int icc_load_display(RendererContext *ctx, SDL_Window *window);
void cal_drop(RendererContext *ctx);
int hdr_refresh(RendererContext *ctx, SDL_Window *window);

#if LACHESIS_HAVE_VULKAN
int vk_backend_create(RendererContext *ctx, SDL_Window *window,
                      AVDictionary *opt);
void vk_backend_destroy(RendererContext *ctx);
int vk_render_node(PFN_vkGetInstanceProcAddr get_proc_addr, VkInstance inst,
                   VkPhysicalDevice phys, char *buf, size_t size);
int list_vk_devices_standalone(GpuDeviceNames names, enum GpuClass *classes);
#endif

#if LACHESIS_HAVE_OPENGL
int gl_backend_create(RendererContext *ctx, SDL_Window *window,
                      AVDictionary *opt);
void gl_backend_destroy(RendererContext *ctx);
void gl_pin_current(RendererContext *ctx);
void gl_unpin_current(RendererContext *ctx);
int gl_num_attempts(const AVDictionary *opt);
const char *gl_apply_profile_hints(int attempt, const AVDictionary *opt);
#endif

#if LACHESIS_HAVE_D3D11
int d3d11_backend_create(RendererContext *ctx, SDL_Window *window,
                         AVDictionary *opt);
void d3d11_backend_destroy(RendererContext *ctx);
void d3d11_touch_frame(RendererContext *ctx, const struct pl_frame *in);
bool map_d3d11_frame(RendererContext *ctx, const AVFrame *frame,
                     struct pl_frame *out);
IDXGIFactory1 *dxgi_open_factory(void);
int dxgi_list_adapters(IDXGIFactory1 *factory, GpuDeviceNames names,
                       enum GpuClass *classes, IDXGIAdapter1 **adapters);
#endif

#endif /* LACHESIS_RENDERER_INTERNAL_H */
