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

#include <libavutil/attributes.h>

#include <SDL3/SDL.h>

#include "lachesis_config.h"

#if LACHESIS_HAVE_WAYLAND

int wayland_activate(SDL_Window *window, const char *token);

#else

static inline int wayland_activate(SDL_Window *win av_unused, const char *token av_unused) {
    return 0;
}

#endif

#endif // LACHESIS_WAYLAND_H
