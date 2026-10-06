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

#ifndef LACHESIS_ASS_H
#define LACHESIS_ASS_H

#include <stddef.h>

#include <ass/ass.h>

#include <SDL3/SDL.h>

#define LASS_UI_FAMILY "Selawik"
#define LASS_SYMBOL_FAMILY "lachesis-osd-symbols"
#define LASS_EMOJI_FAMILY "Noto Emoji"

#define LASS_SUB_RES_H 720.0
#define LASS_SUB_FONT_SIZE 60.0
#define LASS_SUB_OUTLINE 2.4
#define LASS_SUB_MARGIN_V 30.0
#define LASS_SUB_MARGIN_X 40.0

ASS_Library *lass_library_new(int extract_fonts);
void lass_library_free(ASS_Library *lib);

void lass_renderer_setup(ASS_Renderer *renderer, const char *family);

typedef struct LassBounds {
    int x0, y0, x1, y1;
} LassBounds;

int lass_bounds(const ASS_Image *img, int w, int h, LassBounds *out);
int lass_ink_bounds(const ASS_Image *img, int w, int h, LassBounds *out);

void lass_blend(SDL_Surface *dst, const ASS_Image *img, int x, int y);

char *lass_strdup(const char *s);
void lass_free(void *p);

size_t lass_escape(char *out, size_t outsz, const char *text);

typedef const char *(*LassStyleFont)(const char *name, size_t len, void *ctx);

int lass_route_emoji(const char *in, const char *font, LassStyleFont lookup,
                     void *ctx, char *out, size_t outsz);

#endif /* LACHESIS_ASS_H */
