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

#include "lachesis_config.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include <SDL3/SDL.h>

#include <wayland-client.h>

#include "generated/wayland/color-management-v1.h"
#include "generated/wayland/xdg-activation-v1.h"
#include "generated/wayland/xdg-decoration-unstable-v1.h"
#include "generated/wayland/xdg-shell.h"
#include "generated/wayland/xdg-toplevel-icon-v1.h"

#include "lachesis_log.h"
#include "lachesis_wayland.h"

#define WM_BASE_VERSION 7
#ifdef WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION
#define COLOR_MANAGER_VERSION 3
#else
#define COLOR_MANAGER_VERSION 1
#endif

#define SYNC_ROUNDTRIPS 8

static struct {
    struct wl_display *display;
    int bound;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct xdg_activation_v1 *activation;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wl_shm *shm;
    struct wp_color_manager_v1 *color_manager;
} wl;

typedef struct Toplevel Toplevel;

typedef struct ColorQuery {
    Toplevel *t;
    int later;
    struct wp_image_description_v1 *description;
    struct wp_image_description_info_v1 *info;
    int finished;
    float headroom;
    int icc_fd;
    uint32_t icc_size;
} ColorQuery;

struct Toplevel {
    SDL_Window *window;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wp_color_management_surface_feedback_v1 *color_feedback;
    struct wl_buffer **icon_buffers;
    int icon_buffer_count;

    struct {
        int w, h;
        int fullscreen, maximized, tiled, suspended, resizing;
    } pending;

    int fullscreen;
    int maximized;
    int tiled;
    int suspended;
    int configured;
    int answer_pending;

    int last_w, last_h;
    int want_w, want_h;
    int want_pending;
    int w, h;

    ColorQuery *color_query;
    float headroom;
    int icc_fd;
    uint32_t icc_size;
};

static Toplevel *top;

static Toplevel *owned(SDL_Window *window) {
    return top && window && top->window == window ? top : NULL;
}

static int size_locked(const Toplevel *t) {
    return t->fullscreen || t->maximized || t->tiled;
}

static void push_window_event(Toplevel *t, Uint32 type) {
    SDL_Event event;

    SDL_zero(event);
    event.type = type;
    event.window.windowID = SDL_GetWindowID(t->window);
    SDL_PushEvent(&event);
}

static void apply_size(Toplevel *t, int w, int h) {
    if (w <= 0 || h <= 0 || (w == t->w && h == t->h)) {
        return;
    }
    t->w = w;
    t->h = h;
    SDL_SetWindowSize(t->window, w, h);
}

static void wm_base_ping(void *data av_unused, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static uint32_t bind_version(uint32_t offered, const struct wl_interface *interface,
                             uint32_t handled) {
    uint32_t version = FFMIN(offered, (uint32_t)interface->version);

    return FFMIN(version, handled);
}

static void registry_global(void *data av_unused, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wl.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                      bind_version(version, &xdg_wm_base_interface, WM_BASE_VERSION));
        xdg_wm_base_add_listener(wl.wm_base, &wm_base_listener, NULL);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        wl.decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface, xdg_activation_v1_interface.name)) {
        wl.activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    } else if (!strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name)) {
        wl.icon_manager = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wp_color_manager_v1_interface.name)) {
        wl.color_manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface,
                                            bind_version(version, &wp_color_manager_v1_interface, COLOR_MANAGER_VERSION));
    }
}

static void registry_global_remove(void *data av_unused,
                                   struct wl_registry *registry av_unused,
                                   uint32_t name av_unused) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void to_default_queue(void *proxy) {
    if (proxy) {
        wl_proxy_set_queue(proxy, NULL);
    }
}

static int bind_globals(struct wl_display *display) {
    struct wl_event_queue *queue;
    struct wl_display *wrapper;
    struct wl_registry *registry;

    if (wl.bound && wl.display == display) {
        return wl.wm_base && wl.decoration_manager;
    }
    memset(&wl, 0, sizeof(wl));
    wl.display = display;
    wl.bound = 1;

    queue = wl_display_create_queue(display);
    if (!queue) {
        return 0;
    }
    wrapper = wl_proxy_create_wrapper(display);
    if (!wrapper) {
        wl_event_queue_destroy(queue);
        return 0;
    }
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip_queue(display, queue);
    wl_registry_destroy(registry);

    to_default_queue(wl.wm_base);
    to_default_queue(wl.decoration_manager);
    to_default_queue(wl.activation);
    to_default_queue(wl.icon_manager);
    to_default_queue(wl.shm);
    to_default_queue(wl.color_manager);
    wl_event_queue_destroy(queue);

    if (!wl.wm_base) {
        log_verbose("The compositor has no xdg_wm_base; SDL keeps the window.\n");
    } else if (!wl.decoration_manager) {
        log_verbose("The compositor draws no decorations; SDL keeps the window.\n");
    }

    return wl.wm_base && wl.decoration_manager;
}

static void toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel av_unused,
                               int32_t w, int32_t h, struct wl_array *states) {
    Toplevel *t = data;
    uint32_t *state;

    t->pending.w = w;
    t->pending.h = h;
    t->pending.fullscreen = 0;
    t->pending.maximized = 0;
    t->pending.tiled = 0;
    t->pending.suspended = 0;
    t->pending.resizing = 0;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            t->pending.fullscreen = 1;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            t->pending.maximized = 1;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            t->pending.resizing = 1;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            t->pending.tiled = 1;
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            t->pending.suspended = 1;
            break;
        default:
            break;
        }
    }
}

static void toplevel_close(void *data av_unused,
                           struct xdg_toplevel *xdg_toplevel av_unused) {
    SDL_Event event;

    SDL_zero(event);
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

static void toplevel_configure_bounds(void *data av_unused,
                                      struct xdg_toplevel *xdg_toplevel av_unused,
                                      int32_t w av_unused, int32_t h av_unused) {
}

static void toplevel_wm_capabilities(void *data av_unused,
                                     struct xdg_toplevel *xdg_toplevel av_unused,
                                     struct wl_array *capabilities av_unused) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void surface_configure(void *data, struct xdg_surface *xdg_surface,
                              uint32_t serial) {
    Toplevel *t = data;
    int was_fullscreen = t->fullscreen;
    int was_suspended = t->suspended;
    int w = t->pending.w;
    int h = t->pending.h;
    int repeat = w == t->last_w && h == t->last_h;

    t->fullscreen = t->pending.fullscreen;
    t->maximized = t->pending.maximized;
    t->tiled = t->pending.tiled;
    t->suspended = t->pending.suspended;
    t->last_w = w;
    t->last_h = h;

    if (size_locked(t)) {
        if (w <= 0 || h <= 0) {
            w = t->w;
            h = t->h;
        }
    } else if (w > 0 && h > 0 &&
               (t->pending.resizing || (!repeat && !t->want_pending))) {
        t->want_w = w;
        t->want_h = h;
        t->want_pending = 0;
    } else {
        w = t->want_w;
        h = t->want_h;
        t->want_pending = 0;
    }

    xdg_surface_ack_configure(xdg_surface, serial);
    apply_size(t, w, h);

    if (t->configured) {
        if (t->fullscreen != was_fullscreen) {
            push_window_event(t, t->fullscreen ? SDL_EVENT_WINDOW_ENTER_FULLSCREEN : SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
        }
        if (t->suspended != was_suspended) {
            push_window_event(t, t->suspended ? SDL_EVENT_WINDOW_OCCLUDED : SDL_EVENT_WINDOW_EXPOSED);
        }
    }
    t->configured = 1;
    t->answer_pending = 0;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void color_query_free(ColorQuery *q) {
    if (!q) {
        return;
    }
    if (q->info) {
        wp_image_description_info_v1_destroy(q->info);
    }
    if (q->description) {
        wp_image_description_v1_destroy(q->description);
    }
    if (q->icc_fd >= 0) {
        close(q->icc_fd);
    }
    av_free(q);
}

static void color_adopt(Toplevel *t, ColorQuery *q) {
    float headroom = q->headroom > 1.0f ? q->headroom : 1.0f;
    int icc_changed = q->icc_size || t->icc_size;
    int hdr_changed = headroom != t->headroom;

    if (t->icc_fd >= 0) {
        close(t->icc_fd);
    }
    t->icc_fd = q->icc_fd;
    t->icc_size = q->icc_size;
    q->icc_fd = -1;
    t->headroom = headroom;

    if (q->later) {
        if (icc_changed) {
            push_window_event(t, SDL_EVENT_WINDOW_ICCPROF_CHANGED);
        }
        if (hdr_changed) {
            push_window_event(t, SDL_EVENT_WINDOW_HDR_STATE_CHANGED);
        }
    }
}

static void color_finish(ColorQuery *q, int ok) {
    q->finished = 1;
    if (!q->later) {
        return;
    }
    if (ok) {
        color_adopt(q->t, q);
    }
    if (q->t->color_query == q) {
        q->t->color_query = NULL;
    }
    color_query_free(q);
}

static void info_done(void *data,
                      struct wp_image_description_info_v1 *info av_unused) {
    color_finish(data, 1);
}

static void info_icc_file(void *data,
                          struct wp_image_description_info_v1 *info av_unused,
                          int32_t icc, uint32_t icc_size) {
    ColorQuery *q = data;

    if (q->icc_fd >= 0) {
        close(q->icc_fd);
    }
    q->icc_fd = icc;
    q->icc_size = icc_size;
}

static void info_primaries(void *data av_unused,
                           struct wp_image_description_info_v1 *info av_unused,
                           int32_t r_x av_unused, int32_t r_y av_unused,
                           int32_t g_x av_unused, int32_t g_y av_unused,
                           int32_t b_x av_unused, int32_t b_y av_unused,
                           int32_t w_x av_unused, int32_t w_y av_unused) {
}

static void info_primaries_named(void *data av_unused,
                                 struct wp_image_description_info_v1 *info av_unused,
                                 uint32_t primaries av_unused) {
}

static void info_tf_power(void *data av_unused,
                          struct wp_image_description_info_v1 *info av_unused,
                          uint32_t eexp av_unused) {
}

static void info_tf_named(void *data av_unused,
                          struct wp_image_description_info_v1 *info av_unused,
                          uint32_t tf av_unused) {
}

static void info_luminances(void *data,
                            struct wp_image_description_info_v1 *info av_unused,
                            uint32_t min_lum av_unused, uint32_t max_lum,
                            uint32_t reference_lum) {
    ColorQuery *q = data;

    q->headroom = reference_lum ? (float)max_lum / (float)reference_lum : 1.0f;
}

static void info_target_primaries(void *data av_unused,
                                  struct wp_image_description_info_v1 *info av_unused,
                                  int32_t r_x av_unused, int32_t r_y av_unused,
                                  int32_t g_x av_unused, int32_t g_y av_unused,
                                  int32_t b_x av_unused, int32_t b_y av_unused,
                                  int32_t w_x av_unused, int32_t w_y av_unused) {
}

static void info_target_luminance(void *data av_unused,
                                  struct wp_image_description_info_v1 *info av_unused,
                                  uint32_t min_lum av_unused,
                                  uint32_t max_lum av_unused) {
}

static void info_target_max_cll(void *data av_unused,
                                struct wp_image_description_info_v1 *info av_unused,
                                uint32_t max_cll av_unused) {
}

static void info_target_max_fall(void *data av_unused,
                                 struct wp_image_description_info_v1 *info av_unused,
                                 uint32_t max_fall av_unused) {
}

static const struct wp_image_description_info_v1_listener info_listener = {
    .done = info_done,
    .icc_file = info_icc_file,
    .primaries = info_primaries,
    .primaries_named = info_primaries_named,
    .tf_power = info_tf_power,
    .tf_named = info_tf_named,
    .luminances = info_luminances,
    .target_primaries = info_target_primaries,
    .target_luminance = info_target_luminance,
    .target_max_cll = info_target_max_cll,
    .target_max_fall = info_target_max_fall,
};

static void description_failed(void *data,
                               struct wp_image_description_v1 *description av_unused,
                               uint32_t cause av_unused, const char *msg) {
    log_verbose("The compositor has no preferred image description: %s.\n",
                msg ? msg : "no reason");
    color_finish(data, 0);
}

static void description_ready2(void *data,
                               struct wp_image_description_v1 *description,
                               uint32_t identity_hi av_unused,
                               uint32_t identity_lo av_unused) {
    ColorQuery *q = data;

    q->info = wp_image_description_v1_get_information(description);
    wp_image_description_info_v1_add_listener(q->info, &info_listener, q);
}

static void description_ready(void *data,
                              struct wp_image_description_v1 *description,
                              uint32_t identity) {
    description_ready2(data, description, 0, identity);
}

static const struct wp_image_description_v1_listener description_listener = {
    .failed = description_failed,
    .ready = description_ready,
#ifdef WP_IMAGE_DESCRIPTION_V1_READY2_SINCE_VERSION
    .ready2 = description_ready2,
#endif
};

static ColorQuery *color_query_new(Toplevel *t, int later) {
    ColorQuery *q = av_mallocz(sizeof(*q));

    if (q) {
        q->t = t;
        q->later = later;
        q->headroom = 1.0f;
        q->icc_fd = -1;
    }

    return q;
}

static void color_query_now(Toplevel *t) {
    struct wl_event_queue *queue;
    struct wl_proxy *wrapper;
    ColorQuery *q;

    if (!t->color_feedback || !(q = color_query_new(t, 0))) {
        return;
    }
    queue = wl_display_create_queue(wl.display);
    wrapper = queue ? wl_proxy_create_wrapper(t->color_feedback) : NULL;
    if (!wrapper) {
        if (queue) {
            wl_event_queue_destroy(queue);
        }
        color_query_free(q);
        return;
    }
    wl_proxy_set_queue(wrapper, queue);
    q->description = wp_color_management_surface_feedback_v1_get_preferred(
        (struct wp_color_management_surface_feedback_v1 *)wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wp_image_description_v1_add_listener(q->description, &description_listener, q);

    while (!q->finished && wl_display_dispatch_queue(wl.display, queue) >= 0) {
    }
    if (q->finished && q->info) {
        color_adopt(t, q);
    }
    color_query_free(q);
    wl_event_queue_destroy(queue);
}

static void color_query_later(Toplevel *t) {
    ColorQuery *q;

    color_query_free(t->color_query);
    t->color_query = q = color_query_new(t, 1);
    if (!q) {
        return;
    }
    q->description = wp_color_management_surface_feedback_v1_get_preferred(t->color_feedback);
    wp_image_description_v1_add_listener(q->description, &description_listener, q);
}

static void feedback_preferred_changed(void *data,
                                       struct wp_color_management_surface_feedback_v1 *feedback av_unused,
                                       uint32_t identity av_unused) {
    color_query_later(data);
}

#ifdef WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION
static void feedback_preferred_changed2(void *data,
                                        struct wp_color_management_surface_feedback_v1 *feedback av_unused,
                                        uint32_t identity_hi av_unused,
                                        uint32_t identity_lo av_unused) {
    color_query_later(data);
}
#endif

static const struct wp_color_management_surface_feedback_v1_listener feedback_listener = {
    .preferred_changed = feedback_preferred_changed,
#ifdef WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION
    .preferred_changed2 = feedback_preferred_changed2,
#endif
};

static const char *app_id(void) {
    const char *id = SDL_GetHint(SDL_HINT_APP_ID);

    if (!id || !*id) {
        id = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING);
    }

    return id && *id ? id : "lachesis";
}

SDL_Window *wayland_create_window(const char *title, int w, int h,
                                  SDL_WindowFlags flags) {
    const char *driver = SDL_GetCurrentVideoDriver();
    struct wl_display *display;
    SDL_PropertiesID props;
    SDL_Window *window;
    Toplevel *t;

    if (top || !driver || strcmp(driver, "wayland")) {
        return NULL;
    }
    display = SDL_GetPointerProperty(SDL_GetGlobalProperties(),
                                     SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, NULL);
    if (!display || !bind_globals(display)) {
        return NULL;
    }

    props = SDL_CreateProperties();
    if (!props) {
        return NULL;
    }
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          flags & ~(SDL_WINDOW_HIDDEN | SDL_WINDOW_FULLSCREEN));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN, true);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
        return NULL;
    }

    t = av_mallocz(sizeof(*t));
    if (!t) {
        SDL_DestroyWindow(window);
        return NULL;
    }
    t->window = window;
    t->surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    SDL_GetWindowSize(window, &t->w, &t->h);
    t->want_w = t->w;
    t->want_h = t->h;
    t->headroom = 1.0f;
    t->icc_fd = -1;
    if (!t->surface) {
        av_free(t);
        SDL_DestroyWindow(window);
        return NULL;
    }
    top = t;

    t->xdg_surface = xdg_wm_base_get_xdg_surface(wl.wm_base, t->surface);
    xdg_surface_add_listener(t->xdg_surface, &surface_listener, t);
    t->xdg_toplevel = xdg_surface_get_toplevel(t->xdg_surface);
    xdg_toplevel_add_listener(t->xdg_toplevel, &toplevel_listener, t);
    xdg_toplevel_set_app_id(t->xdg_toplevel, app_id());
    if (title) {
        xdg_toplevel_set_title(t->xdg_toplevel, title);
    }
    if (wl.decoration_manager) {
        t->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl.decoration_manager, t->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(t->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (flags & SDL_WINDOW_FULLSCREEN) {
        xdg_toplevel_set_fullscreen(t->xdg_toplevel, NULL);
    }
    if (wl.color_manager) {
        t->color_feedback = wp_color_manager_v1_get_surface_feedback(wl.color_manager, t->surface);
        wp_color_management_surface_feedback_v1_add_listener(t->color_feedback, &feedback_listener, t);
    }
    wl_surface_commit(t->surface);
    t->answer_pending = 1;
    if (!wayland_sync(window) || !t->configured) {
        log_warn("The compositor never configured the window.\n");
        wayland_destroy_window(window);
        return NULL;
    }
    color_query_now(t);
    log_verbose("lachesis manages the Wayland toplevel.\n");

    return window;
}

static void forget_icon(Toplevel *t) {
    for (int i = 0; i < t->icon_buffer_count; i++) {
        wl_buffer_destroy(t->icon_buffers[i]);
    }
    av_freep(&t->icon_buffers);
    t->icon_buffer_count = 0;
}

void wayland_destroy_window(SDL_Window *window) {
    Toplevel *t = owned(window);

    if (t) {
        color_query_free(t->color_query);
        if (t->color_feedback) {
            wp_color_management_surface_feedback_v1_destroy(t->color_feedback);
        }
        if (t->icc_fd >= 0) {
            close(t->icc_fd);
        }
        forget_icon(t);
        if (t->decoration) {
            zxdg_toplevel_decoration_v1_destroy(t->decoration);
        }
        xdg_toplevel_destroy(t->xdg_toplevel);
        xdg_surface_destroy(t->xdg_surface);
        av_free(t);
        top = NULL;
    }
    SDL_DestroyWindow(window);
}

int wayland_owns(SDL_Window *window) {
    return owned(window) != NULL;
}

int wayland_undecorated(void) {
    return wl.wm_base && !wl.decoration_manager;
}

void wayland_set_fullscreen(SDL_Window *window, int fullscreen) {
    Toplevel *t = owned(window);

    if (!t) {
        return;
    }
    wl_display_roundtrip(wl.display);
    if (!t->answer_pending && !fullscreen == !t->fullscreen) {
        return;
    }
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(t->xdg_toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(t->xdg_toplevel);
    }
    t->answer_pending = 1;
}

int wayland_sync(SDL_Window *window) {
    Toplevel *t = owned(window);

    if (!t) {
        return 0;
    }
    for (int i = 0; t->answer_pending && i < SYNC_ROUNDTRIPS; i++) {
        if (wl_display_roundtrip(wl.display) < 0) {
            break;
        }
    }

    return !t->answer_pending;
}

int wayland_fullscreen(SDL_Window *window) {
    Toplevel *t = owned(window);

    return t && t->fullscreen;
}

int wayland_size_locked(SDL_Window *window) {
    Toplevel *t = owned(window);

    return t && size_locked(t);
}

int wayland_occluded(SDL_Window *window) {
    Toplevel *t = owned(window);

    return t && t->suspended;
}

void wayland_set_size(SDL_Window *window, int w, int h) {
    Toplevel *t = owned(window);

    if (!t || w <= 0 || h <= 0) {
        return;
    }
    t->want_w = w;
    t->want_h = h;
    t->want_pending = size_locked(t);
    if (!t->want_pending) {
        apply_size(t, w, h);
    }
}

void wayland_set_title(SDL_Window *window, const char *title) {
    Toplevel *t = owned(window);

    if (t && title) {
        xdg_toplevel_set_title(t->xdg_toplevel, title);
    }
}

void wayland_activate(SDL_Window *window, const char *token) {
    Toplevel *t = owned(window);

    if (t && wl.activation && token) {
        xdg_activation_v1_activate(wl.activation, token, t->surface);
        wl_display_flush(wl.display);
    }
}

void wayland_show(SDL_Window *window) {
    const char *token;

    if (!owned(window)) {
        return;
    }
    if (!wl.activation || !SDL_GetHintBoolean(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, true)) {
        SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "XDG_ACTIVATION_TOKEN");
        return;
    }
    token = SDL_getenv("XDG_ACTIVATION_TOKEN");
    if (token) {
        wayland_activate(window, token);
        SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "XDG_ACTIVATION_TOKEN");
    } else {
        SDL_RaiseWindow(window);
    }
}

static int anonymous_file(size_t size) {
    const char *dir;
    char *path;
    int fd = -1;

#ifdef MFD_CLOEXEC
    fd = memfd_create("lachesis-icon", MFD_CLOEXEC);
#endif
    if (fd < 0 && (dir = getenv("XDG_RUNTIME_DIR")) && dir[0] == '/' &&
        (path = av_asprintf("%s/lachesis-icon-XXXXXX", dir))) {
        fd = mkstemp(path);
        if (fd >= 0) {
            unlink(path);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        av_free(path);
    }
    if (fd >= 0 && ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        fd = -1;
    }

    return fd;
}

static int compare_icon_size(const void *a, const void *b) {
    const SDL_Surface *x = *(SDL_Surface *const *)a;
    const SDL_Surface *y = *(SDL_Surface *const *)b;

    return x->w - y->w;
}

int wayland_set_icon(SDL_Window *window, SDL_Surface *icon) {
    Toplevel *t = owned(window);
    struct xdg_toplevel_icon_v1 *xdg_icon;
    struct wl_shm_pool *pool;
    SDL_Surface **images;
    size_t total = 0;
    size_t offset = 0;
    uint8_t *map;
    int count = 0;
    int fd;

    if (!t) {
        return 0;
    }
    if (!wl.icon_manager || !wl.shm || !icon) {
        return 1;
    }
    images = SDL_GetSurfaceImages(icon, &count);
    if (!images) {
        return 1;
    }
    for (int i = 0; i < count; i++) {
        if (images[i]->w == images[i]->h) {
            total += (size_t)images[i]->w * images[i]->h * 4;
        }
    }
    fd = total ? anonymous_file(total) : -1;
    if (fd < 0) {
        SDL_free(images);
        return 1;
    }
    map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        SDL_free(images);
        return 1;
    }

    forget_icon(t);
    t->icon_buffers = av_calloc(count, sizeof(*t->icon_buffers));
    pool = wl_shm_create_pool(wl.shm, fd, (int32_t)total);
    xdg_icon = xdg_toplevel_icon_manager_v1_create_icon(wl.icon_manager);
    SDL_qsort(images, count, sizeof(*images), compare_icon_size);
    for (int i = 0; i < count && t->icon_buffers; i++) {
        SDL_Surface *image = images[i];
        int size = image->w;
        int scale = size / icon->w;
        struct wl_buffer *buffer;

        if (image->w != image->h || !scale) {
            continue;
        }
        if (!SDL_PremultiplyAlpha(size, size, image->format, image->pixels, image->pitch,
                                  SDL_PIXELFORMAT_ARGB8888, map + offset, size * 4, true)) {
            continue;
        }
        buffer = wl_shm_pool_create_buffer(pool, (int32_t)offset, size, size,
                                           size * 4, WL_SHM_FORMAT_ARGB8888);
        xdg_toplevel_icon_v1_add_buffer(xdg_icon, buffer, scale);
        t->icon_buffers[t->icon_buffer_count++] = buffer;
        offset += (size_t)size * size * 4;
    }
    xdg_toplevel_icon_manager_v1_set_icon(wl.icon_manager, t->xdg_toplevel, xdg_icon);
    xdg_toplevel_icon_v1_destroy(xdg_icon);
    wl_shm_pool_destroy(pool);
    munmap(map, total);
    close(fd);
    SDL_free(images);

    return 1;
}

void *wayland_icc_profile(SDL_Window *window, size_t *size) {
    Toplevel *t = owned(window);
    void *data = NULL;
    void *map;

    *size = 0;
    if (!t || t->icc_fd < 0 || !t->icc_size) {
        return NULL;
    }
    map = mmap(NULL, t->icc_size, PROT_READ, MAP_PRIVATE, t->icc_fd, 0);
    if (map == MAP_FAILED) {
        return NULL;
    }
    data = av_memdup(map, t->icc_size);
    munmap(map, t->icc_size);
    *size = t->icc_size;

    return data;
}

float wayland_hdr_headroom(SDL_Window *window) {
    Toplevel *t = owned(window);

    return t ? t->headroom : 1.0f;
}

void wayland_uninit(void) {
    if (top || !wl.bound) {
        return;
    }
    if (wl.color_manager) {
        wp_color_manager_v1_destroy(wl.color_manager);
    }
    if (wl.icon_manager) {
        xdg_toplevel_icon_manager_v1_destroy(wl.icon_manager);
    }
    if (wl.activation) {
        xdg_activation_v1_destroy(wl.activation);
    }
    if (wl.decoration_manager) {
        zxdg_decoration_manager_v1_destroy(wl.decoration_manager);
    }
    if (wl.shm) {
        wl_shm_destroy(wl.shm);
    }
    if (wl.wm_base) {
        xdg_wm_base_destroy(wl.wm_base);
    }
    memset(&wl, 0, sizeof(wl));
}
