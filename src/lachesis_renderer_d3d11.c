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
#include "lachesis_log.h"
#include "lachesis_renderer_internal.h"
/* clang-format on */

#if LACHESIS_HAVE_D3D11

#include <stdlib.h>
#include <string.h>

#include <libavutil/buffer.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

static int force_software(const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "software", NULL, 0);
    int force = 0;

    if (entry && entry->value) {
        force = strtol(entry->value, NULL, 10) != 0;
    }

    return force;
}

typedef HRESULT(WINAPI *create_dxgi_factory1_fn)(REFIID riid, void **factory);

IDXGIFactory1 *dxgi_open_factory(void) {
    static create_dxgi_factory1_fn create_factory;
    static int looked_up;
    IDXGIFactory1 *factory = NULL;

    if (!looked_up) {
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");

        looked_up = 1;
        if (dxgi) {
            create_factory = (create_dxgi_factory1_fn)(void *)GetProcAddress(
                dxgi, "CreateDXGIFactory1");
        }
    }
    if (!create_factory ||
        FAILED(create_factory(&IID_IDXGIFactory1, (void **)&factory))) {
        return NULL;
    }

    return factory;
}

static void dxgi_describe(IDXGIAdapter1 *adapter, char name[256],
                          enum GpuClass *class) {
    DXGI_ADAPTER_DESC1 desc;

    name[0] = '\0';
    *class = GPU_CLASS_ANY;

    if (FAILED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
        return;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 256, NULL,
                             NULL)) {
        name[0] = '\0';
    }
    name[255] = '\0';

    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        return;
    }
    *class = desc.DedicatedVideoMemory > 0 ? GPU_CLASS_DISCRETE
                                           : GPU_CLASS_INTEGRATED;
}

int dxgi_list_adapters(IDXGIFactory1 *factory, GpuDeviceNames names,
                       enum GpuClass *classes,
                       IDXGIAdapter1 **adapters) {
    int num = 0;

    while (num < MAX_GPU_DEVICES) {
        IDXGIAdapter1 *adapter = NULL;

        if (IDXGIFactory1_EnumAdapters1(factory, (UINT)num, &adapter) !=
            S_OK) {
            break;
        }
        dxgi_describe(adapter, names[num], &classes[num]);
        adapters[num] = adapter;
        num++;
    }

    return num;
}

static IDXGIAdapter1 *dxgi_preferred_adapter(IDXGIFactory1 *factory,
                                             enum GpuClass want) {
#ifdef __IDXGIFactory6_INTERFACE_DEFINED__
    IDXGIFactory6 *factory6 = NULL;
    IDXGIAdapter1 *adapter = NULL;
    DXGI_GPU_PREFERENCE preference =
        want == GPU_CLASS_DISCRETE ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                   : DXGI_GPU_PREFERENCE_MINIMUM_POWER;

    if (FAILED(IDXGIFactory1_QueryInterface(factory, &IID_IDXGIFactory6,
                                            (void **)&factory6))) {
        return NULL;
    }
    if (IDXGIFactory6_EnumAdapterByGpuPreference(
            factory6, 0, preference, &IID_IDXGIAdapter1,
            (void **)&adapter) != S_OK) {
        adapter = NULL;
    }
    IDXGIFactory6_Release(factory6);

    return adapter;
#else
    (void)factory;
    (void)want;

    return NULL;
#endif
}

static IDXGIAdapter1 *d3d11_pick_adapter(void) {
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapters[MAX_GPU_DEVICES] = {0};
    IDXGIAdapter1 *chosen = NULL;
    enum GpuClass classes[MAX_GPU_DEVICES];
    enum GpuClass chosen_class;
    enum GpuClass want;
    GpuDeviceNames names;
    char chosen_name[256];
    int num;
    int match;

    if (!renderer_want_device) {
        return NULL;
    }

    factory = dxgi_open_factory();
    if (!factory) {
        return NULL;
    }

    num = dxgi_list_adapters(factory, names, classes, adapters);
    renderer_report_gpu_devices("Direct3D 11", names, num, 1);

    want = renderer_gpu_class_request(renderer_want_device);
    if (want != GPU_CLASS_ANY) {
        chosen = dxgi_preferred_adapter(factory, want);
        if (chosen) {
            dxgi_describe(chosen, chosen_name, &chosen_class);
            log_verbose("Direct3D 11: Windows picked '%s' as the %s GPU.\n",
                        chosen_name,
                        want == GPU_CLASS_DISCRETE ? "high performance"
                                                   : "low power");
        }
    }

    match = chosen ? -1 : renderer_match_gpu_device(names, classes, num, renderer_want_device);
    if (match >= 0) {
        chosen = adapters[match];
        IDXGIAdapter1_AddRef(chosen);
    } else if (!chosen) {
        log_warn("No Direct3D 11 device matches '%s'.\n", renderer_want_device);
        renderer_report_gpu_devices("Direct3D 11", names, num, 0);
    }

    for (int i = 0; i < num; i++) {
        IDXGIAdapter1_Release(adapters[i]);
    }
    IDXGIFactory1_Release(factory);

    return chosen;
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
        if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                 ctx->device_name,
                                 (int)sizeof(ctx->device_name), NULL, NULL)) {
            ctx->device_name[0] = '\0';
        }
        ctx->device_name[sizeof(ctx->device_name) - 1] = '\0';
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
    ctx->d3d11_multithread = multithread;
}

static void d3d11_lock(void *lock_ctx) {
    ID3D10Multithread_Enter((ID3D10Multithread *)lock_ctx);
}

static void d3d11_unlock(void *lock_ctx) {
    ID3D10Multithread_Leave((ID3D10Multithread *)lock_ctx);
}

static int d3d11_shader_bind_usable(RendererContext *ctx) {
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 64,
        .Height = 64,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_NV12,
        .SampleDesc = {.Count = 1},
        .ArraySize = 2,
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE,
    };
    ID3D11Texture2D *tex = NULL;

    if (SUCCEEDED(ID3D11Device_CreateTexture2D(ctx->placebo_d3d11->device, &desc,
                                               NULL, &tex))) {
        ID3D11Texture2D_Release(tex);
        return 1;
    }

    desc.BindFlags = D3D11_BIND_DECODER;
    if (FAILED(ID3D11Device_CreateTexture2D(ctx->placebo_d3d11->device, &desc,
                                            NULL, &tex))) {
        return 1;
    }
    ID3D11Texture2D_Release(tex);

    return 0;
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

    if (ctx->d3d11_multithread) {
        hwctx->lock = d3d11_lock;
        hwctx->unlock = d3d11_unlock;
        hwctx->lock_ctx = ctx->d3d11_multithread;
    }

    if (d3d11_shader_bind_usable(ctx)) {
        hwctx->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    } else {
    }

    ret = av_hwdevice_ctx_init(ctx->hw_device_ref);
    if (ret < 0) {
        av_buffer_unref(&ctx->hw_device_ref);
        return ret;
    }

    return 0;
}

int d3d11_backend_create(RendererContext *ctx, SDL_Window *window,
                         AVDictionary *opt) {
    const AVDictionaryEntry *entry;
    IDXGIAdapter1 *adapter;
    HWND hwnd;
    int software = force_software(opt);
    int present_timing = 1;
    int w, h;

    entry = av_dict_get(opt, "present_timing", NULL, 0);
    if (entry && entry->value && !strtol(entry->value, NULL, 10)) {
        present_timing = 0;
    }

    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                        NULL);
    if (!hwnd) {
        return AVERROR_EXTERNAL;
    }

    adapter = software ? NULL : d3d11_pick_adapter();

    /* clang-format off */
    ctx->placebo_d3d11 = pl_d3d11_create(ctx->log_ctx,
                                         pl_d3d11_params(
                                             .debug = enable_debug(opt),
                                             .adapter = (IDXGIAdapter *)adapter,
                                             .allow_software = software || renderer_allow_software_gpu,
                                             .force_software = software,
                                             .flags = software ? 0 : D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                             .min_feature_level = D3D_FEATURE_LEVEL_10_0, ));
    /* clang-format on */
    if (adapter) {
        IDXGIAdapter1_Release(adapter);
    }
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

    if (present_timing) {
        d3dpresent_attach(ctx->swapchain);
    }

    snprintf(ctx->api_name, sizeof(ctx->api_name), "Direct3D 11");
    d3d11_read_device_name(ctx);

    if (ctx->placebo_d3d11->software && !software) {
        log_warn("Fell back to WARP.\n");
    }

    if (!ctx->placebo_d3d11->software) {
        int hw_ret = d3d11_create_hw_device(ctx);

        if (hw_ret < 0) {
        }
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

static void d3d11_drop_pool(RendererContext *ctx, struct D3D11ViewPool *pool) {
    for (unsigned i = 0; i < pool->num_views; i++) {
        pl_tex_destroy(ctx->gpu, &pool->views[i]);
    }
    av_freep(&pool->views);
    pool->num_views = 0;
    pool->texture = NULL;
    pool->serial = 0;
}

static void d3d11_drop_views(RendererContext *ctx) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->d3d11_pools); i++) {
        d3d11_drop_pool(ctx, &ctx->d3d11_pools[i]);
    }
}

static struct D3D11ViewPool *d3d11_view_pool(RendererContext *ctx,
                                             ID3D11Texture2D *texture,
                                             unsigned need) {
    struct D3D11ViewPool *victim = NULL;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->d3d11_pools); i++) {
        struct D3D11ViewPool *pool = &ctx->d3d11_pools[i];

        if (pool->texture == texture && pool->num_views == need) {
            pool->serial = ctx->d3d11_serial;
            return pool;
        }
        if (pool->serial == ctx->d3d11_serial && pool->texture) {
            continue;
        }
        if (!victim || pool->serial < victim->serial) {
            victim = pool;
        }
    }

    if (!victim) {
        return NULL;
    }

    d3d11_drop_pool(ctx, victim);
    victim->views = av_calloc(need, sizeof(*victim->views));
    if (!victim->views) {
        return NULL;
    }
    victim->num_views = need;
    victim->texture = texture;
    victim->serial = ctx->d3d11_serial;

    return victim;
}

void d3d11_touch_frame(RendererContext *ctx, const struct pl_frame *in) {
    pl_tex tex = in->num_planes > 0 ? in->planes[0].texture : NULL;

    if (!tex) {
        return;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(ctx->d3d11_pools); i++) {
        struct D3D11ViewPool *pool = &ctx->d3d11_pools[i];

        for (unsigned j = 0; j < pool->num_views; j++) {
            if (pool->views[j] == tex) {
                pool->serial = ctx->d3d11_serial;
                return;
            }
        }
    }
}

bool map_d3d11_frame(RendererContext *ctx, const AVFrame *frame,
                     struct pl_frame *out) {
    const AVHWFramesContext *hwfc;
    const AVPixFmtDescriptor *desc;
    ID3D11Texture2D *texture = (ID3D11Texture2D *)frame->data[0];
    unsigned slice = (unsigned)(intptr_t)frame->data[1];
    struct D3D11ViewPool *pool;
    DXGI_FORMAT view_fmt[4];
    D3D11_TEXTURE2D_DESC tex_desc;
    unsigned need;
    int planes;

    if (!texture || !frame->hw_frames_ctx) {
        return false;
    }

    hwfc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    desc = av_pix_fmt_desc_get(hwfc->sw_format);
    if (!desc) {
        return false;
    }

    ID3D11Texture2D_GetDesc(texture, &tex_desc);
    planes = d3d11_plane_view_formats(tex_desc.Format, view_fmt);
    if (!planes || slice >= tex_desc.ArraySize) {
        return false;
    }

    need = tex_desc.ArraySize * (unsigned)planes;
    pool = d3d11_view_pool(ctx, texture, need);
    if (!pool) {
        return false;
    }

    pl_frame_from_avframe(out, frame);
    if (out->num_planes != planes) {
        return false;
    }

    for (int i = 0; i < planes; i++) {
        pl_tex *slot = &pool->views[slice * (unsigned)planes + i];

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

void d3d11_backend_destroy(RendererContext *ctx) {
    d3dpresent_shutdown();
    d3d11_drop_views(ctx);
    av_buffer_unref(&ctx->hw_device_ref);
    if (ctx->d3d11_multithread) {
        ID3D10Multithread_Release(ctx->d3d11_multithread);
        ctx->d3d11_multithread = NULL;
    }
    pl_swapchain_destroy(&ctx->swapchain);
    pl_d3d11_destroy(&ctx->placebo_d3d11);
}

#endif /* LACHESIS_HAVE_D3D11 */
