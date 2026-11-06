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

// clang-format off
#include "lachesis_log.h"
#include "lachesis_renderer_internal.h"
// clang-format on

#if LACHESIS_HAVE_OPENGL

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libavutil/avstring.h>
#include <libavutil/macros.h>

#ifdef _WIN32
#define LACHESIS_APIENTRY __stdcall
#else
#define LACHESIS_APIENTRY
#endif

enum {
    LACHESIS_GL_VENDOR = 0x1F00,
    LACHESIS_GL_RENDERER = 0x1F01,
    LACHESIS_GL_VERSION = 0x1F02,
    LACHESIS_GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    LACHESIS_EGL_NONE = 0x3038,
    LACHESIS_EGL_VENDOR = 0x3053,
    LACHESIS_EGL_VERSION = 0x3054,
};

typedef const unsigned char *(LACHESIS_APIENTRY *gl_get_string_fn)(unsigned);
typedef const char *(LACHESIS_APIENTRY *egl_query_string_fn)(void *, int32_t);
typedef void(LACHESIS_APIENTRY *egl_debug_fn)(unsigned, const char *, int32_t,
                                              void *, void *, const char *);
typedef int32_t(LACHESIS_APIENTRY *egl_debug_control_fn)(egl_debug_fn,
                                                         const intptr_t *);

static const struct gl_profile {
    const char *name;
    int profile;
    int major;
    int minor;
    int newest_first;
    int prefer_egl;
    int angle;
} gl_profiles[] = {
    {"OpenGL 3.3 core (EGL)", SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, 1, 1, 0},
    {"OpenGL 3.3 core", SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, 1, 0, 0},
    {"OpenGL 3.2 core", SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, 0, 0, 0},
    {"OpenGL ES 3.0", SDL_GL_CONTEXT_PROFILE_ES, 3, 0, 0, 1, 0},
    {"OpenGL ES 3.0 (ANGLE)", SDL_GL_CONTEXT_PROFILE_ES, 3, 0, 0, 1, 1},
};

#define GL_NUM_PROFILES ((int)FF_ARRAY_ELEMS(gl_profiles))

static const int gl_core_versions[][2] = {
    {4, 6},
    {4, 5},
    {4, 4},
    {4, 3},
    {4, 2},
    {4, 1},
    {4, 0},
};

static const struct gl_profile *gl_attempt;
static SDL_AtomicInt egl_listening;
static SDL_SpinLock egl_error_lock;
static char egl_error[384];

static const char *egl_error_name(unsigned error) {
    static const char *const names[] = {
        "EGL_SUCCESS",
        "EGL_NOT_INITIALIZED",
        "EGL_BAD_ACCESS",
        "EGL_BAD_ALLOC",
        "EGL_BAD_ATTRIBUTE",
        "EGL_BAD_CONFIG",
        "EGL_BAD_CONTEXT",
        "EGL_BAD_CURRENT_SURFACE",
        "EGL_BAD_DISPLAY",
        "EGL_BAD_MATCH",
        "EGL_BAD_NATIVE_PIXMAP",
        "EGL_BAD_NATIVE_WINDOW",
        "EGL_BAD_PARAMETER",
        "EGL_BAD_SURFACE",
        "EGL_CONTEXT_LOST",
    };

    if (error >= 0x3000 && error - 0x3000 < FF_ARRAY_ELEMS(names)) {
        return names[error - 0x3000];
    }
    switch (error) {
    case 0x321B:
        return "EGL_BAD_STREAM_KHR";
    case 0x321C:
        return "EGL_BAD_STATE_KHR";
    case 0x322B:
        return "EGL_BAD_DEVICE_EXT";
    default:
        return NULL;
    }
}

static void egl_forget_error(void) {
    SDL_LockSpinlock(&egl_error_lock);
    egl_error[0] = '\0';
    SDL_UnlockSpinlock(&egl_error_lock);
}

static void LACHESIS_APIENTRY egl_debug(unsigned error, const char *command,
                                        int32_t type av_unused,
                                        void *thread_label av_unused,
                                        void *object_label av_unused,
                                        const char *message) {
    const char *name = egl_error_name(error);
    char code[16];
    char line[sizeof(egl_error)];
    size_t len;

    if (!SDL_GetAtomicInt(&egl_listening)) {
        return;
    }
    if (!command || !*command) {
        command = "EGL";
    }
    if (!strncmp(command, "eglQuery", 8) ||
        !strcmp(command, "eglGetConfigAttrib")) {
        return;
    }
    if (!name) {
        snprintf(code, sizeof(code), "0x%x", error);
        name = code;
    }
    if (message && *message && strcmp(message, command)) {
        snprintf(line, sizeof(line), "%s from %s: %s", name, command, message);
    } else {
        snprintf(line, sizeof(line), "%s from %s", name, command);
    }
    len = strlen(line);
    while (len && strchr("\n. ", line[len - 1])) {
        line[--len] = '\0';
    }
    log_verbose("EGL: %s.\n", line);

    SDL_LockSpinlock(&egl_error_lock);
    memcpy(egl_error, line, len + 1);
    SDL_UnlockSpinlock(&egl_error_lock);
}

static void egl_listen(void) {
    egl_debug_control_fn control =
        (egl_debug_control_fn)SDL_EGL_GetProcAddress("eglDebugMessageControlKHR");

    if (control) {
        control(egl_debug, NULL);
    }
}

static SDL_EGLAttrib *SDLCALL egl_platform_attribs(void *userdata av_unused) {
    SDL_EGLAttrib *attribs = SDL_malloc(sizeof(*attribs));

    egl_listen();
    if (attribs) {
        attribs[0] = LACHESIS_EGL_NONE;
    }

    return attribs;
}

static SDL_EGLint *SDLCALL egl_surface_attribs(void *userdata av_unused,
                                               SDL_EGLDisplay display av_unused,
                                               SDL_EGLConfig config av_unused) {
    SDL_EGLint *attribs = SDL_malloc(sizeof(*attribs));

    egl_listen();
    egl_forget_error();
    if (attribs) {
        attribs[0] = LACHESIS_EGL_NONE;
    }

    return attribs;
}

const char *gl_window_error(const char *detail, char *buf, size_t size) {
    char last[sizeof(egl_error)];

    SDL_LockSpinlock(&egl_error_lock);
    memcpy(last, egl_error, sizeof(last));
    SDL_UnlockSpinlock(&egl_error_lock);

    if (!last[0]) {
        return detail;
    }
    if (detail && *detail) {
        snprintf(buf, size, "%s (the EGL driver's last error was %s)", detail, last);
    } else {
        snprintf(buf, size, "the EGL driver's last error was %s", last);
    }

    return buf;
}

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
    gl_attempt = p;

    if (!SDL_getenv(SDL_HINT_VIDEO_FORCE_EGL)) {
        SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, p->prefer_egl ? "1" : "0");
    }
    if (!SDL_getenv(SDL_HINT_OPENGL_ES_DRIVER)) {
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, p->angle ? "1" : "0");
    }

    egl_forget_error();
    SDL_SetAtomicInt(&egl_listening, 1);

    SDL_GL_ResetAttributes();
    SDL_EGL_SetAttributeCallbacks(egl_platform_attribs, egl_surface_attribs,
                                  NULL, NULL);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, p->profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, p->major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, p->minor);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    // No destination alpha.
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

#endif // LACHESIS_HAVE_DRM_NODES

static const char *gl_string(unsigned name) {
    gl_get_string_fn get_string =
        (gl_get_string_fn)SDL_GL_GetProcAddress("glGetString");
    const unsigned char *s = get_string ? get_string(name) : NULL;

    return s && *s ? (const char *)s : NULL;
}

static int gl_current_version(void) {
    const char *version = gl_string(LACHESIS_GL_VERSION);
    int major, minor;

    if (!version || sscanf(version, "%d.%d", &major, &minor) != 2) {
        return 0;
    }

    return major * 10 + minor;
}

static SDL_GLContext gl_create_context(SDL_Window *window, int *major,
                                       int *minor) {
    const struct gl_profile *p = gl_attempt;
    SDL_GLContext context, newer;

    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    context = SDL_GL_CreateContext(window);
    if (!context || !p || !p->newest_first ||
        gl_current_version() != *major * 10 + *minor) {
        return context;
    }

    for (size_t i = 0; i < FF_ARRAY_ELEMS(gl_core_versions); i++) {
        int want_major = gl_core_versions[i][0];
        int want_minor = gl_core_versions[i][1];

        if (want_major * 10 + want_minor <= *major * 10 + *minor) {
            break;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, want_major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, want_minor);
        newer = SDL_GL_CreateContext(window);
        if (newer) {
            SDL_GL_DestroyContext(context);
            context = newer;
            *major = want_major;
            *minor = want_minor;
            break;
        }
        log_verbose("OpenGL: no %d.%d core context: %s.\n", want_major,
                    want_minor, SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, *major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, *minor);
    SDL_ClearError();

    return context;
}

static void gl_log_context(SDL_EGLDisplay egl_display, int major, int minor) {
    const char *version = gl_string(LACHESIS_GL_VERSION);
    const char *glsl = gl_string(LACHESIS_GL_SHADING_LANGUAGE_VERSION);
    const char *vendor = gl_string(LACHESIS_GL_VENDOR);
    char asked[32];

    if (gl_attempt && gl_attempt->profile == SDL_GL_CONTEXT_PROFILE_ES) {
        snprintf(asked, sizeof(asked), "ES %d.%d", major, minor);
    } else {
        snprintf(asked, sizeof(asked), "%d.%d core", major, minor);
    }
    log_verbose("OpenGL: asked for %s and got %s (GLSL %s) from %s.\n", asked,
                version ? version : "an unknown version",
                glsl ? glsl : "unknown", vendor ? vendor : "an unknown vendor");

    if (egl_display) {
        egl_query_string_fn query =
            (egl_query_string_fn)SDL_EGL_GetProcAddress("eglQueryString");
        const char *egl_version = query ? query(egl_display, LACHESIS_EGL_VERSION) : NULL;
        const char *egl_vendor = query ? query(egl_display, LACHESIS_EGL_VENDOR) : NULL;

        log_verbose("OpenGL: through EGL %s from %s.\n",
                    egl_version ? egl_version : "of an unknown version",
                    egl_vendor ? egl_vendor : "an unknown vendor");
    } else if (gl_video_driver_is("x11")) {
        log_verbose("OpenGL: through GLX.\n");
    }
}

int gl_backend_create(RendererContext *ctx, SDL_Window *window,
                      AVDictionary *opt) {
    AVDictionaryEntry *entry;
    SDL_EGLDisplay egl_display;
    int max_glsl = 0;
    int major = 0, minor = 0;
    int w, h;

    ctx->gl_context = gl_create_context(window, &major, &minor);
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
    gl_log_context(egl_display, major, minor);
    SDL_SetAtomicInt(&egl_listening, 0);
#if LACHESIS_HAVE_CUDA_GL
    cuda_gl_init(ctx, gl_string(LACHESIS_GL_VENDOR));
#endif

    // clang-format off
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
    // clang-format on
    if (!ctx->placebo_gl) {
        return AVERROR_EXTERNAL;
    }

    ctx->gpu = ctx->placebo_gl->gpu;

    // clang-format off
    ctx->swapchain = pl_opengl_create_swapchain(ctx->placebo_gl,
                                                pl_opengl_swapchain_params(
                                                    .swap_buffers = gl_swap_buffers,
                                                    .framebuffer.flipped = false,
                                                    .priv = ctx, ));
    // clang-format on
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
        const char *name = gl_string(LACHESIS_GL_RENDERER);

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
#if LACHESIS_HAVE_CUDA_GL
    if (ctx->cuda_gl) {
        gl_pin_current(ctx);
        cuda_gl_uninit(ctx);
        gl_unpin_current(ctx);
    }
#endif
    pl_swapchain_destroy(&ctx->swapchain);
    pl_opengl_destroy(&ctx->placebo_gl);

    if (ctx->gl_context) {
        SDL_GL_DestroyContext(ctx->gl_context);
        ctx->gl_context = NULL;
    }
}

#endif // LACHESIS_HAVE_OPENGL
