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

#ifndef LACHESIS_VIEW360_H
#define LACHESIS_VIEW360_H

#include <SDL3/SDL.h>

#include "lachesis_renderer.h"

int view360_draw(SDL_Renderer *renderer, SDL_Texture *texture,
                 const SDL_Rect *rect, enum Vk360Layout layout,
                 float yaw, float pitch, float hfov, int flip_v);

void view360_free(void);

#endif /* LACHESIS_VIEW360_H */
