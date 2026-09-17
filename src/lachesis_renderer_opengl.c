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

#if LACHESIS_HAVE_OPENGL

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libavutil/avstring.h>
#include <libavutil/macros.h>

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

int gl_num_attempts(const AVDictionary *opt) {
    int order[GL_NUM_PROFILES];

    return gl_profile_order(order, opt);
}

const char *gl_apply_profile_hints(int attempt, const AVDictionary *opt) {
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
    SDL_GL_SwapWindow(((RendererContext *)priv)->window);
}

static bool gl_make_current(void *priv) {
    RendererContext *ctx = priv;

    if (ctx->gl_pinned_by == SDL_GetCurrentThreadID()) {
        return true;
    }

    return SDL_GL_MakeCurrent(ctx->window, ctx->gl_context);
}

static void gl_release_current(void *priv) {
    RendererContext *ctx = priv;

    if (ctx->gl_pinned_by == SDL_GetCurrentThreadID()) {
        return;
    }
    SDL_GL_MakeCurrent(ctx->window, NULL);
}

void gl_pin_current(RendererContext *ctx) {
    if (ctx->api.backend != RENDERER_API_OPENGL || ctx->gl_pinned_by) {
        return;
    }
    if (SDL_GL_MakeCurrent(ctx->window, ctx->gl_context)) {
        ctx->gl_pinned_by = SDL_GetCurrentThreadID();
    }
}

void gl_unpin_current(RendererContext *ctx) {
    if (ctx->api.backend != RENDERER_API_OPENGL ||
        ctx->gl_pinned_by != SDL_GetCurrentThreadID()) {
        return;
    }
    ctx->gl_pinned_by = 0;
    SDL_GL_MakeCurrent(ctx->window, NULL);
}

#ifdef LACHESIS_HAVE_DRM_NODES

static int card_node_to_render_node(const char *card, char *buf, size_t size) {
    const char *base = strrchr(card, '/');
    char path[128];
    struct dirent *ent;
    DIR *dir;
    int ret = AVERROR(ENOENT);

    buf[0] = '\0';
    base = base ? base + 1 : card;
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/drm", base);

    dir = opendir(path);
    if (!dir) {
        return AVERROR(ENOSYS);
    }
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "renderD", 7)) {
            continue;
        }
        av_strlcpy(buf, "/dev/dri/", size);
        av_strlcat(buf, ent->d_name, size);
        ret = 0;
        break;
    }
    closedir(dir);

    return ret;
}

static int gl_render_node(RendererContext *ctx, char *buf, size_t size) {
    enum {
        LACHESIS_EGL_DEVICE = 0x322C,
        LACHESIS_EGL_DRM_DEVICE_FILE = 0x3233,
        LACHESIS_EGL_DRM_RENDER_NODE_FILE = 0x3377,
    };
    typedef unsigned int egl_bool;
    typedef egl_bool (*query_display_attrib_fn)(void *, int32_t, intptr_t *);
    typedef const char *(*query_device_string_fn)(void *, int32_t);
    query_display_attrib_fn query_display;
    query_device_string_fn query_device;
    const char *path;
    intptr_t device = 0;
    struct stat st;

    if (!ctx->gl_egl_display) {
        return AVERROR(ENOSYS);
    }
    query_display = (query_display_attrib_fn)SDL_GL_GetProcAddress(
        "eglQueryDisplayAttribEXT");
    query_device = (query_device_string_fn)SDL_GL_GetProcAddress(
        "eglQueryDeviceStringEXT");
    if (!query_display || !query_device) {
        SDL_ClearError();
        return AVERROR(ENOSYS);
    }

    if (!query_display(ctx->gl_egl_display, LACHESIS_EGL_DEVICE, &device) ||
        !device) {
        return AVERROR(ENOSYS);
    }

    path = query_device((void *)device, LACHESIS_EGL_DRM_RENDER_NODE_FILE);
    if (path && path[0] && strstr(path, "render")) {
        av_strlcpy(buf, path, size);
    } else {
        path = query_device((void *)device, LACHESIS_EGL_DRM_DEVICE_FILE);
        if (!path || !strstr(path, "/card") ||
            card_node_to_render_node(path, buf, size) < 0) {
            return AVERROR(ENOSYS);
        }
    }

    if (stat(buf, &st) < 0 || !S_ISCHR(st.st_mode)) {
        buf[0] = '\0';
        return AVERROR(ENOENT);
    }

    return 0;
}

#endif /* LACHESIS_HAVE_DRM_NODES */

int gl_backend_create(RendererContext *ctx, SDL_Window *window,
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
    if (!SDL_GL_GetSwapInterval(&ctx->gl_swap_interval)) {
        ctx->gl_swap_interval = 1;
    }

    egl_display = SDL_EGL_GetCurrentDisplay();
    if (!egl_display) {
        SDL_ClearError();
    }
    ctx->gl_egl_display = egl_display;

    /* clang-format off */
    ctx->placebo_gl = pl_opengl_create(ctx->log_ctx,
                                       pl_opengl_params(
                                           .get_proc_addr = gl_get_proc_addr,
                                           .debug = enable_debug(opt),
                                           .allow_software = renderer_allow_software_gpu,
                                           .max_glsl_version = max_glsl,
                                           .egl_display = egl_display,
                                           .make_current = gl_make_current,
                                           .release_current = gl_release_current,
                                           .priv = ctx, ));
    if (!ctx->placebo_gl && egl_display) {
        egl_display = NULL;
        ctx->gl_egl_display = NULL;
        ctx->placebo_gl = pl_opengl_create(ctx->log_ctx,
                                           pl_opengl_params(
                                               .get_proc_addr = gl_get_proc_addr,
                                               .debug = enable_debug(opt),
                                               .allow_software = renderer_allow_software_gpu,
                                               .max_glsl_version = max_glsl,
                                               .make_current = gl_make_current,
                                               .release_current = gl_release_current,
                                               .priv = ctx, ));
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
                                                    .priv = ctx, ));
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

    if (SDL_GL_MakeCurrent(window, ctx->gl_context)) {
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
        SDL_GL_MakeCurrent(window, NULL);
    }

#ifdef LACHESIS_HAVE_DRM_NODES
    if (SDL_GL_MakeCurrent(window, ctx->gl_context)) {
        if (gl_render_node(ctx, ctx->gl_drm_node,
                           sizeof(ctx->gl_drm_node)) < 0) {
            ctx->gl_drm_node[0] = '\0';
        } else {
            log_verbose("OpenGL: rendering on %s.\n", ctx->gl_drm_node);
        }
        SDL_GL_MakeCurrent(window, NULL);
    }
#endif

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

    SDL_GL_MakeCurrent(window, NULL);

    return 0;
}

void gl_backend_destroy(RendererContext *ctx) {
    pl_swapchain_destroy(&ctx->swapchain);
    pl_opengl_destroy(&ctx->placebo_gl);

    if (ctx->gl_context) {
        SDL_GL_DestroyContext(ctx->gl_context);
        ctx->gl_context = NULL;
    }
}

#endif /* LACHESIS_HAVE_OPENGL */
