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

#include <stdint.h>
#include <string.h>

#include <libavutil/attributes.h>

#include <SDL3/SDL.h>

#include <wayland-client.h>

#include "generated/wayland/xdg-activation-v1.h"

#include "lachesis_log.h"
#include "lachesis_wayland.h"

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version av_unused) {
    struct xdg_activation_v1 **activation = data;

    if (!*activation && !strcmp(interface, xdg_activation_v1_interface.name)) {
        *activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
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

int wayland_activate(SDL_Window *window, const char *token) {
    const char *driver = SDL_GetCurrentVideoDriver();
    struct xdg_activation_v1 *activation = NULL;
    struct wl_display *display, *wrapper;
    struct wl_registry *registry;
    struct wl_event_queue *queue;
    struct wl_surface *surface;
    int activated = 0;

    if (!window || !token || !*token || !driver || strcmp(driver, "wayland")) {
        return 0;
    }
    display = SDL_GetPointerProperty(SDL_GetGlobalProperties(),
                                     SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, NULL);
    surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                     SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (!display || !surface || !(queue = wl_display_create_queue(display))) {
        return 0;
    }

    wrapper = wl_proxy_create_wrapper(display);
    if (wrapper) {
        wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
        registry = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        if (registry) {
            wl_registry_add_listener(registry, &registry_listener, &activation);
            wl_display_roundtrip_queue(display, queue);
            wl_registry_destroy(registry);
        }
    }
    if (activation) {
        xdg_activation_v1_activate(activation, token, surface);
        xdg_activation_v1_destroy(activation);
        wl_display_flush(display);
        activated = 1;
    } else {
        log_verbose("The compositor has no xdg_activation_v1.\n");
    }
    wl_event_queue_destroy(queue);

    return activated;
}
