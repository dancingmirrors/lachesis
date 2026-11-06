/*
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

// clang-format off
#include "lachesis_log.h"
#include "lachesis_renderer_internal.h"
// clang-format on

#if LACHESIS_HAVE_CUDA_GL

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef LACHESIS_HAVE_DRM_NODES
#include <unistd.h>
#endif

#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

#ifdef _WIN32
#define CUDAAPI __stdcall
#define CUDA_LIBRARY "nvcuda.dll"
#else
#define CUDAAPI
#define CUDA_LIBRARY "libcuda.so.1"
#endif

enum {
    CUDA_SUCCESS = 0,
    CU_DEVICE_ATTRIBUTE_PCI_BUS_ID = 33,
    CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID = 34,
    CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID = 50,
    CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD = 2,
    CU_MEMORYTYPE_DEVICE = 2,
    CU_MEMORYTYPE_ARRAY = 3,
};

typedef int CUresult;
typedef int CUdevice;
typedef uintptr_t CUdeviceptr;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef struct CUarray_st *CUarray;
typedef struct CUgraphicsResource_st *CUgraphicsResource;

typedef struct CUDA_MEMCPY2D {
    size_t srcXInBytes;
    size_t srcY;
    unsigned int srcMemoryType;
    const void *srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes;
    size_t dstY;
    unsigned int dstMemoryType;
    void *dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes;
    size_t Height;
} CUDA_MEMCPY2D;

typedef CUresult CUDAAPI tcuInit(unsigned int flags);
typedef CUresult CUDAAPI tcuGetErrorName(CUresult error, const char **name);
typedef CUresult CUDAAPI tcuDeviceGetCount(int *count);
typedef CUresult CUDAAPI tcuDeviceGet(CUdevice *device, int ordinal);
typedef CUresult CUDAAPI tcuDeviceGetAttribute(int *value, int attribute,
                                               CUdevice device);
typedef CUresult CUDAAPI tcuCtxPushCurrent(CUcontext context);
typedef CUresult CUDAAPI tcuCtxPopCurrent(CUcontext *context);
typedef CUresult CUDAAPI tcuGraphicsGLRegisterImage(CUgraphicsResource *resource,
                                                    unsigned int image,
                                                    unsigned int target,
                                                    unsigned int flags);
typedef CUresult CUDAAPI tcuGraphicsUnregisterResource(CUgraphicsResource resource);
typedef CUresult CUDAAPI tcuGraphicsMapResources(unsigned int count,
                                                 CUgraphicsResource *resources,
                                                 CUstream stream);
typedef CUresult CUDAAPI tcuGraphicsUnmapResources(unsigned int count,
                                                   CUgraphicsResource *resources,
                                                   CUstream stream);
typedef CUresult CUDAAPI tcuGraphicsSubResourceGetMappedArray(
    CUarray *array, CUgraphicsResource resource, unsigned int index,
    unsigned int level);
typedef CUresult CUDAAPI tcuMemcpy2DAsync(const CUDA_MEMCPY2D *copy,
                                          CUstream stream);

typedef struct CudaFunctions {
    tcuInit *cuInit;
    tcuGetErrorName *cuGetErrorName;
    tcuDeviceGetCount *cuDeviceGetCount;
    tcuDeviceGet *cuDeviceGet;
    tcuDeviceGetAttribute *cuDeviceGetAttribute;
    tcuCtxPushCurrent *cuCtxPushCurrent;
    tcuCtxPopCurrent *cuCtxPopCurrent;
    tcuGraphicsGLRegisterImage *cuGraphicsGLRegisterImage;
    tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;
    tcuGraphicsMapResources *cuGraphicsMapResources;
    tcuGraphicsUnmapResources *cuGraphicsUnmapResources;
    tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;
    tcuMemcpy2DAsync *cuMemcpy2DAsync;
} CudaFunctions;

typedef struct CudaDeviceContext {
    CUcontext cuda_ctx;
    CUstream stream;
} CudaDeviceContext;

#include <libplacebo/utils/upload.h>

#define CUDA_GL_SETS (4 + LACHESIS_MAX_MIX_FRAMES)

typedef struct CudaGlSet {
    const pl_tex *key;
    AVBufferRef *device;
    enum AVPixelFormat sw_format;
    pl_tex tex[4];
    CUgraphicsResource res[4];
    AVFrame *shown;
} CudaGlSet;

struct CudaGl {
    SDL_Mutex *lock;
    SDL_SharedObject *lib;
    CudaFunctions cu;
    int loaded;
    SDL_AtomicInt device_state;
    int said_main_thread;
    int said_mapped;
    CudaGlSet sets[CUDA_GL_SETS];
};

static int cuda_ok(const CudaGl *p, CUresult res, const char *what) {
    const char *name = NULL;

    if (res == CUDA_SUCCESS) {
        return 1;
    }
    p->cu.cuGetErrorName(res, &name);
    log_verbose("CUDA: %s failed: %s.\n", what, name ? name : "unknown error");

    return 0;
}

static int cuda_gl_load_functions(CudaGl *p) {
    CudaFunctions *cu = &p->cu;
    SDL_SharedObject *lib = p->lib;

#define CUDA_LOAD(name, symbol) \
    (cu->name = (t##name *)SDL_LoadFunction(lib, symbol)) != NULL
    return CUDA_LOAD(cuInit, "cuInit") &&
        CUDA_LOAD(cuGetErrorName, "cuGetErrorName") &&
        CUDA_LOAD(cuDeviceGetCount, "cuDeviceGetCount") &&
        CUDA_LOAD(cuDeviceGet, "cuDeviceGet") &&
        CUDA_LOAD(cuDeviceGetAttribute, "cuDeviceGetAttribute") &&
        CUDA_LOAD(cuCtxPushCurrent, "cuCtxPushCurrent_v2") &&
        CUDA_LOAD(cuCtxPopCurrent, "cuCtxPopCurrent_v2") &&
        CUDA_LOAD(cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage") &&
        CUDA_LOAD(cuGraphicsUnregisterResource,
                  "cuGraphicsUnregisterResource") &&
        CUDA_LOAD(cuGraphicsMapResources, "cuGraphicsMapResources") &&
        CUDA_LOAD(cuGraphicsUnmapResources, "cuGraphicsUnmapResources") &&
        CUDA_LOAD(cuGraphicsSubResourceGetMappedArray,
                  "cuGraphicsSubResourceGetMappedArray") &&
        CUDA_LOAD(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2");
#undef CUDA_LOAD
}

static int cuda_gl_load(CudaGl *p) {
    int loaded;

    SDL_LockMutex(p->lock);
    if (!p->loaded) {
        p->loaded = -1;
        p->lib = SDL_LoadObject(CUDA_LIBRARY);
        if (!p->lib || !cuda_gl_load_functions(p)) {
            log_verbose("CUDA: %s.\n", SDL_GetError());
            SDL_ClearError();
        } else if (cuda_ok(p, p->cu.cuInit(0), "cuInit")) {
            p->loaded = 1;
        }
        if (p->loaded < 0) {
            if (p->lib) {
                SDL_UnloadObject(p->lib);
                p->lib = NULL;
            }
            log_verbose("OpenGL: CUDA is unavailable.\n");
        }
    }
    loaded = p->loaded > 0;
    SDL_UnlockMutex(p->lock);

    return loaded;
}

void cuda_gl_init(RendererContext *ctx, const char *gl_vendor) {
    CudaGl *p;

    if (!gl_vendor || !strstr(gl_vendor, "NVIDIA")) {
        return;
    }
    p = av_mallocz(sizeof(*p));
    if (!p) {
        return;
    }
    p->lock = SDL_CreateMutex();
    if (!p->lock) {
        av_free(p);
        return;
    }
    for (int i = 0; i < CUDA_GL_SETS; i++) {
        p->sets[i].sw_format = AV_PIX_FMT_NONE;
    }
    ctx->cuda_gl = p;
}

#ifdef LACHESIS_HAVE_DRM_NODES

static int node_pci_address(const char *node, unsigned int pci[3]) {
    const char *base = strrchr(node, '/');
    const char *addr;
    char path[128];
    char link[256];
    unsigned int function;
    ssize_t len;

    snprintf(path, sizeof(path), "/sys/class/drm/%s/device",
             base ? base + 1 : node);
    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0) {
        return 0;
    }
    link[len] = '\0';
    addr = strrchr(link, '/');
    addr = addr ? addr + 1 : link;

    return sscanf(addr, "%x:%x:%x.%x", &pci[0], &pci[1], &pci[2],
                  &function) == 4;
}

static int renderer_cuda_index(CudaGl *p, const unsigned int pci[3]) {
    int count = 0;

    if (!cuda_ok(p, p->cu.cuDeviceGetCount(&count), "cuDeviceGetCount")) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        int domain, bus, slot;
        CUdevice dev;

        if (p->cu.cuDeviceGet(&dev, i) != CUDA_SUCCESS ||
            p->cu.cuDeviceGetAttribute(&domain, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, dev) != CUDA_SUCCESS ||
            p->cu.cuDeviceGetAttribute(&bus, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, dev) != CUDA_SUCCESS ||
            p->cu.cuDeviceGetAttribute(&slot, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, dev) != CUDA_SUCCESS) {
            continue;
        }
        if ((unsigned int)domain == pci[0] && (unsigned int)bus == pci[1] &&
            (unsigned int)slot == pci[2]) {
            return i;
        }
    }

    return -1;
}

static void cuda_gl_open_device(RendererContext *ctx, CudaGl *p,
                                const unsigned int pci[3]) {
    char name[16];
    int index;
    int err;

    if (!cuda_gl_load(p)) {
        return;
    }
    index = renderer_cuda_index(p, pci);
    if (index < 0) {
        log_verbose("OpenGL: no CUDA device is the renderer's GPU.\n");
        return;
    }
    snprintf(name, sizeof(name), "%d", index);
    err = av_hwdevice_ctx_create(&ctx->hw_device_ref, AV_HWDEVICE_TYPE_CUDA,
                                 name, NULL, 0);
    if (err < 0) {
        log_verbose("OpenGL: CUDA device %d is the renderer's GPU but could "
                    "not be opened: %s.\n",
                    index, av_err2str(err));
    } else {
        log_verbose("OpenGL: CUDA device %d is the renderer's GPU.\n", index);
    }
}

#endif // LACHESIS_HAVE_DRM_NODES

int cuda_gl_device(RendererContext *ctx, AVBufferRef **dev) {
#ifdef LACHESIS_HAVE_DRM_NODES
    CudaGl *p = ctx->cuda_gl;
    unsigned int pci[3];

    *dev = NULL;
    if (!p || !ctx->gl_drm_node[0] ||
        !node_pci_address(ctx->gl_drm_node, pci)) {
        return AVERROR(ENOSYS);
    }
    if (SDL_IsMainThread()) {
        if (!SDL_GetAtomicInt(&p->device_state) && !p->said_main_thread) {
            p->said_main_thread = 1;
            log_verbose("OpenGL: CUDA is only started in the background.\n");
        }
    } else if (SDL_CompareAndSwapAtomicInt(&p->device_state, 0, 1)) {
        cuda_gl_open_device(ctx, p, pci);
        SDL_SetAtomicInt(&p->device_state, 2);
    }
    if (SDL_GetAtomicInt(&p->device_state) == 2 && ctx->hw_device_ref) {
        *dev = ctx->hw_device_ref;
        return 0;
    }
#else
    (void)ctx;
    *dev = NULL;
#endif

    return AVERROR(ENOSYS);
}

int cuda_gl_takes(RendererContext *ctx, const AVFrame *frame) {
    return ctx->cuda_gl && frame->format == AV_PIX_FMT_CUDA &&
        frame->hw_frames_ctx && cuda_gl_load(ctx->cuda_gl);
}

static void cuda_gl_release(RendererContext *ctx, CudaGlSet *set) {
    CudaGl *p = ctx->cuda_gl;

    if (set->device && p->loaded > 0) {
        const AVHWDeviceContext *dc = (const AVHWDeviceContext *)set->device->data;
        const CudaDeviceContext *hw = dc->hwctx;
        CUcontext dummy;

        if (p->cu.cuCtxPushCurrent(hw->cuda_ctx) == CUDA_SUCCESS) {
            for (int i = 0; i < 4; i++) {
                if (set->res[i]) {
                    p->cu.cuGraphicsUnregisterResource(set->res[i]);
                }
            }
            p->cu.cuCtxPopCurrent(&dummy);
        }
    }
    for (int i = 0; i < 4; i++) {
        set->res[i] = NULL;
        pl_tex_destroy(ctx->gpu, &set->tex[i]);
    }
    av_frame_free(&set->shown);
    av_buffer_unref(&set->device);
    set->sw_format = AV_PIX_FMT_NONE;
}

void cuda_gl_release_frames(RendererContext *ctx) {
    CudaGl *p = ctx->cuda_gl;

    if (!p) {
        return;
    }
    for (int i = 0; i < CUDA_GL_SETS; i++) {
        av_frame_free(&p->sets[i].shown);
    }
}

void cuda_gl_uninit(RendererContext *ctx) {
    CudaGl *p = ctx->cuda_gl;

    if (!p) {
        return;
    }
    for (int i = 0; i < CUDA_GL_SETS; i++) {
        cuda_gl_release(ctx, &p->sets[i]);
    }
    av_buffer_unref(&ctx->hw_device_ref);
    if (p->lib) {
        SDL_UnloadObject(p->lib);
    }
    SDL_DestroyMutex(p->lock);
    av_freep(&ctx->cuda_gl);
}

static CudaGlSet *cuda_gl_set(CudaGl *p, const pl_tex *key) {
    CudaGlSet *unused = NULL;

    for (int i = 0; i < CUDA_GL_SETS; i++) {
        if (p->sets[i].key == key) {
            return &p->sets[i];
        }
        if (!p->sets[i].key && !unused) {
            unused = &p->sets[i];
        }
    }
    if (unused) {
        unused->key = key;
    }

    return unused;
}

static bool cuda_gl_holds(const CudaGlSet *set, const AVFrame *frame) {
    const AVFrame *shown = set->shown;

    if (!shown || !shown->buf[0] || !frame->buf[0] ||
        shown->buf[0]->buffer != frame->buf[0]->buffer ||
        shown->width != frame->width || shown->height != frame->height) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (shown->data[i] != frame->data[i] ||
            shown->linesize[i] != frame->linesize[i]) {
            return false;
        }
    }

    return true;
}

static int cuda_gl_plane(RendererContext *ctx, CudaGlSet *set, int i,
                         pl_fmt fmt, int w, int h) {
    CudaGl *p = ctx->cuda_gl;
    pl_tex tex = set->tex[i];
    unsigned int target = 0;
    unsigned int name;

    if (tex && set->res[i] && tex->params.format == fmt &&
        tex->params.w == w && tex->params.h == h) {
        return 1;
    }
    if (set->res[i]) {
        p->cu.cuGraphicsUnregisterResource(set->res[i]);
        set->res[i] = NULL;
    }
    pl_tex_destroy(ctx->gpu, &set->tex[i]);

    // clang-format off
    set->tex[i] = pl_tex_create(ctx->gpu, pl_tex_params(
                                              .w = w,
                                              .h = h,
                                              .format = fmt,
                                              .sampleable = true,
                                              .blit_src = !!(fmt->caps & PL_FMT_CAP_BLITTABLE), ));
    // clang-format on
    if (!set->tex[i]) {
        return 0;
    }
    name = pl_opengl_unwrap(ctx->gpu, set->tex[i], &target, NULL, NULL);
    if (!name ||
        !cuda_ok(p, p->cu.cuGraphicsGLRegisterImage(&set->res[i], name, target, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD),
                 "cuGraphicsGLRegisterImage")) {
        set->res[i] = NULL;
        pl_tex_destroy(ctx->gpu, &set->tex[i]);
        return 0;
    }

    return 1;
}

static bool cuda_gl_copy(RendererContext *ctx, CudaGlSet *set,
                         const AVFrame *frame, const pl_fmt fmt[4],
                         const struct pl_plane_data data[4], int planes) {
    CudaGl *p = ctx->cuda_gl;
    const AVHWFramesContext *fc =
        (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    const CudaDeviceContext *hw = fc->device_ctx->hwctx;
    CUcontext dummy;
    int mapped = 0;
    int ok = 0;

    if (!cuda_ok(p, p->cu.cuCtxPushCurrent(hw->cuda_ctx), "cuCtxPushCurrent")) {
        return false;
    }

    for (int i = 0; i < planes; i++) {
        if (!cuda_gl_plane(ctx, set, i, fmt[i], data[i].width,
                           data[i].height)) {
            goto done;
        }
    }

    if (!cuda_ok(p, p->cu.cuGraphicsMapResources(planes, set->res, hw->stream),
                 "cuGraphicsMapResources")) {
        goto done;
    }
    mapped = 1;

    for (int i = 0; i < planes; i++) {
        CUDA_MEMCPY2D copy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = (CUdeviceptr)frame->data[i],
            .srcPitch = (size_t)frame->linesize[i],
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .WidthInBytes = (size_t)data[i].width * fmt[i]->texel_size,
            .Height = (size_t)data[i].height,
        };

        if (!cuda_ok(p, p->cu.cuGraphicsSubResourceGetMappedArray(&copy.dstArray, set->res[i], 0, 0),
                     "cuGraphicsSubResourceGetMappedArray") ||
            !cuda_ok(p, p->cu.cuMemcpy2DAsync(&copy, hw->stream),
                     "cuMemcpy2DAsync")) {
            goto done;
        }
    }
    ok = 1;

done:
    if (mapped &&
        !cuda_ok(p, p->cu.cuGraphicsUnmapResources(planes, set->res, hw->stream),
                 "cuGraphicsUnmapResources")) {
        ok = 0;
    }
    p->cu.cuCtxPopCurrent(&dummy);

    return ok;
}

static bool cuda_gl_fill(const CudaGlSet *set, const AVFrame *frame,
                         const struct pl_bit_encoding *bits,
                         int map[4][4], int planes, struct pl_frame *out) {
    struct pl_avframe_priv *priv = calloc(1, sizeof(*priv));
    AVFrame *props = priv ? av_frame_clone(frame) : NULL;

    if (!props) {
        free(priv);
        return false;
    }
    props->format = set->sw_format;

    pl_frame_from_avframe(out, props);
    priv->avframe = props;
    out->user_data = priv;
    out->repr.bits = *bits;

#ifdef PL_HAVE_LAV_DOLBY_VISION
    {
        AVFrameSideData *sd =
            av_frame_get_side_data(props, AV_FRAME_DATA_DOVI_METADATA);

        if (sd) {
            const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;

#if PL_API_VER >= 370
            pl_map_avdovi_metadata(&out->color, &out->repr, &priv->dovi, dovi);
#elif PL_API_VER >= 364
            if (pl_avdovi_metadata_supported(dovi)) {
                pl_map_avdovi_metadata(&out->color, &out->repr, &priv->dovi,
                                       dovi);
            }
#else
            if (av_dovi_get_header(dovi)->disable_residual_flag) {
                pl_map_avdovi_metadata(&out->color, &out->repr, &priv->dovi,
                                       dovi);
            }
#endif
        }
#ifdef PL_HAVE_LIBDOVI
        sd = av_frame_get_side_data(props, AV_FRAME_DATA_DOVI_RPU_BUFFER);
        if (sd) {
            pl_hdr_metadata_from_dovi_rpu(&out->color.hdr, sd->buf->data,
                                          sd->buf->size);
        }
#endif
    }
#endif

    out->num_planes = planes;
    for (int i = 0; i < planes; i++) {
        struct pl_plane *plane = &out->planes[i];

        plane->texture = set->tex[i];
        plane->components = 0;
        for (int c = 0; c < 4; c++) {
            plane->component_mapping[c] = map[i][c];
            if (map[i][c] >= 0) {
                plane->components = c + 1;
            }
        }
    }

    return true;
}

bool cuda_gl_map(RendererContext *ctx, const AVFrame *frame, const pl_tex *key,
                 struct pl_frame *out) {
    CudaGl *p = ctx->cuda_gl;
    const AVHWFramesContext *fc;
    const AVPixFmtDescriptor *desc;
    struct pl_plane_data data[4] = {0};
    struct pl_bit_encoding bits = {0};
    pl_fmt fmt[4] = {0};
    int map[4][4];
    CudaGlSet *set;
    int planes;

    if (!p || frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx ||
        !cuda_gl_load(p)) {
        return false;
    }
    fc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    desc = av_pix_fmt_desc_get(fc->sw_format);
    planes = desc ? pl_plane_data_from_pixfmt(data, &bits, fc->sw_format) : 0;
    set = planes > 0 ? cuda_gl_set(p, key) : NULL;
    if (!set) {
        return false;
    }

    for (int i = 0; i < planes; i++) {
        int chroma = i == 1 || i == 2;

        data[i].width = AV_CEIL_RSHIFT(frame->width,
                                       chroma ? desc->log2_chroma_w : 0);
        data[i].height = AV_CEIL_RSHIFT(frame->height,
                                        chroma ? desc->log2_chroma_h : 0);
        fmt[i] = pl_plane_find_fmt(ctx->gpu, map[i], &data[i]);
        if (!fmt[i] || fmt[i]->type != PL_FMT_UNORM ||
            fmt[i]->texel_size != data[i].pixel_stride ||
            frame->linesize[i] <= 0) {
            return false;
        }
    }

    if (set->device && (set->device->data != fc->device_ref->data || set->sw_format != fc->sw_format)) {
        cuda_gl_release(ctx, set);
    }
    if (!set->device && !(set->device = av_buffer_ref(fc->device_ref))) {
        return false;
    }
    set->sw_format = fc->sw_format;

    if (!cuda_gl_holds(set, frame)) {
        if (!set->shown) {
            set->shown = av_frame_alloc();
        }
        av_frame_unref(set->shown);
        if (!cuda_gl_copy(ctx, set, frame, fmt, data, planes)) {
            return false;
        }
        if (set->shown) {
            av_frame_ref(set->shown, frame);
        }
    }

    if (!cuda_gl_fill(set, frame, &bits, map, planes, out)) {
        return false;
    }
    if (!p->said_mapped) {
        p->said_mapped = 1;
        log_verbose("OpenGL: CUDA copies %s frames into the textures on the "
                    "GPU.\n",
                    av_get_pix_fmt_name(fc->sw_format));
    }

    return true;
}

#endif // LACHESIS_HAVE_CUDA_GL
