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

#ifndef LACHESIS_WAYLAND_H
#define LACHESIS_WAYLAND_H

#include <stddef.h>

#include <libavutil/attributes.h>

#include <SDL3/SDL.h>

#include "lachesis_config.h"

#if LACHESIS_HAVE_WAYLAND

SDL_Window *wayland_create_window(const char *title, int w, int h,
                                  SDL_WindowFlags flags);
void wayland_destroy_window(SDL_Window *window);
int wayland_owns(SDL_Window *window);
int wayland_undecorated(void);

void wayland_set_fullscreen(SDL_Window *window, int fullscreen);
int wayland_sync(SDL_Window *window);
int wayland_fullscreen(SDL_Window *window);
int wayland_size_locked(SDL_Window *window);
int wayland_occluded(SDL_Window *window);

void wayland_set_size(SDL_Window *window, int w, int h);
void wayland_set_title(SDL_Window *window, const char *title);
void wayland_activate(SDL_Window *window, const char *token);
void wayland_show(SDL_Window *window);
int wayland_set_icon(SDL_Window *window, SDL_Surface *icon);

void *wayland_icc_profile(SDL_Window *window, size_t *size);
float wayland_hdr_headroom(SDL_Window *window);

void wayland_uninit(void);

#else

static inline SDL_Window *wayland_create_window(const char *title av_unused, int w av_unused, int h av_unused, SDL_WindowFlags flags av_unused) {
    return NULL;
}
static inline void wayland_destroy_window(SDL_Window *win) {
    SDL_DestroyWindow(win);
}
static inline int wayland_owns(SDL_Window *win av_unused) {
    return 0;
}
static inline int wayland_undecorated(void) {
    return 0;
}
static inline void wayland_set_fullscreen(SDL_Window *win av_unused, int fullscreen av_unused) {
}
static inline int wayland_sync(SDL_Window *win av_unused) {
    return 0;
}
static inline int wayland_fullscreen(SDL_Window *win av_unused) {
    return 0;
}
static inline int wayland_size_locked(SDL_Window *win av_unused) {
    return 0;
}
static inline int wayland_occluded(SDL_Window *win av_unused) {
    return 0;
}
static inline void wayland_set_size(SDL_Window *win av_unused, int w av_unused, int h av_unused) {
}
static inline void wayland_set_title(SDL_Window *win av_unused, const char *title av_unused) {
}
static inline void wayland_activate(SDL_Window *win av_unused, const char *token av_unused) {
}
static inline void wayland_show(SDL_Window *win av_unused) {
}
static inline int wayland_set_icon(SDL_Window *win av_unused, SDL_Surface *icon av_unused) {
    return 0;
}
static inline void *wayland_icc_profile(SDL_Window *win av_unused, size_t *size av_unused) {
    return NULL;
}
static inline float wayland_hdr_headroom(SDL_Window *win av_unused) {
    return 1.0f;
}
static inline void wayland_uninit(void) {
}

#endif

#endif // LACHESIS_WAYLAND_H
