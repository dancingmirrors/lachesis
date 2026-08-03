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

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "lachesis_ass.h"
#include "lachesis_log.h"
#include "lachesis_osd_emoji_font.h"
#include "lachesis_osd_font.h"
#include "lachesis_osd_ui_font.h"

#define LASS_BREAK_RUN 40

static void lass_message_cb(int level, const char *fmt, va_list va, void *data) {
    char buf[512];

    (void)data;

    /* 0 is fatal, 1 is error, 2 is warning, and 4 is info. */
    if (level > 4) {
        return;
    }

    vsnprintf(buf, sizeof(buf), fmt, va);
    if (level <= 1) {
        log_warn("libass: %s\n", buf);
    } else {
        log_verbose("libass: %s\n", buf);
    }
}

static void lass_add_emoji_font(ASS_Library *lib) {
    uLongf len = osd_emoji_font_size;
    unsigned char *buf = av_malloc(len);

    if (!buf) {
        return;
    }
    if (uncompress(buf, &len, osd_emoji_font_deflate,
                   osd_emoji_font_deflate_size) != Z_OK ||
        len != osd_emoji_font_size) {
        log_warn("Could not decompress the bundled emoji font.\n");
        av_free(buf);
        return;
    }

    ass_add_font(lib, "NotoEmoji", (const char *)buf, (int)len);
    av_free(buf);
}

ASS_Library *lass_library_new(int extract_fonts) {
    ASS_Library *lib = ass_library_init();

    if (!lib) {
        log_warn("Could not initialize libass.\n");
        return NULL;
    }

    ass_set_message_cb(lib, lass_message_cb, NULL);
    ass_set_extract_fonts(lib, extract_fonts);

    ass_add_font(lib, "lachesis-ui", (const char *)osd_ui_font_data,
                 (int)osd_ui_font_data_size);
    ass_add_font(lib, "lachesis-osd-symbols", (const char *)osd_font_data,
                 (int)osd_font_size);
    lass_add_emoji_font(lib);

    return lib;
}

void lass_library_free(ASS_Library *lib) {
    if (lib) {
        ass_library_done(lib);
    }
}

char *lass_strdup(const char *s) {
    size_t n = strlen(s) + 1;
#if defined(LIBASS_VERSION) && LIBASS_VERSION >= 0x01703000
    char *p = ass_malloc(n);
#else
    char *p = malloc(n);
#endif

    if (p) {
        memcpy(p, s, n);
    }

    return p;
}

void lass_renderer_setup(ASS_Renderer *renderer, const char *family) {
    ass_set_shaper(renderer, ASS_SHAPING_COMPLEX);
    ass_set_hinting(renderer, ASS_HINTING_NONE);
    ass_set_fonts(renderer, NULL, family ? family : "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
}

int lass_bounds(const ASS_Image *img, int w, int h, LassBounds *out) {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;

    for (const ASS_Image *p = img; p; p = p->next) {
        if (p->w <= 0 || p->h <= 0) {
            continue;
        }
        x0 = FFMIN(x0, p->dst_x);
        y0 = FFMIN(y0, p->dst_y);
        x1 = FFMAX(x1, p->dst_x + p->w);
        y1 = FFMAX(y1, p->dst_y + p->h);
    }
    if (x0 > x1 || y0 > y1) {
        return 0;
    }

    if (w > 0) {
        x0 = FFMAX(x0, 0);
        x1 = FFMIN(x1, w);
    }
    if (h > 0) {
        y0 = FFMAX(y0, 0);
        y1 = FFMIN(y1, h);
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }

    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;

    return 1;
}

int lass_ink_bounds(const ASS_Image *img, int w, int h, LassBounds *out) {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;

    for (const ASS_Image *p = img; p; p = p->next) {
        if (p->w <= 0 || p->h <= 0 || (p->color & 0xFF) == 0xFF) {
            continue;
        }
        for (int y = 0; y < p->h; y++) {
            const unsigned char *row = p->bitmap + (size_t)y * (size_t)p->stride;
            int first = -1, last = -1;

            for (int x = 0; x < p->w; x++) {
                if (row[x]) {
                    first = x;
                    break;
                }
            }
            if (first < 0) {
                continue;
            }
            for (int x = p->w - 1; x >= first; x--) {
                if (row[x]) {
                    last = x;
                    break;
                }
            }
            x0 = FFMIN(x0, p->dst_x + first);
            x1 = FFMAX(x1, p->dst_x + last + 1);
            y0 = FFMIN(y0, p->dst_y + y);
            y1 = FFMAX(y1, p->dst_y + y + 1);
        }
    }
    if (x0 > x1 || y0 > y1) {
        return 0;
    }

    if (w > 0) {
        x0 = FFMAX(x0, 0);
        x1 = FFMIN(x1, w);
    }
    if (h > 0) {
        y0 = FFMAX(y0, 0);
        y1 = FFMIN(y1, h);
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }

    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;

    return 1;
}

typedef struct {
    int r, g, b, a;
} LassShift;

static int lass_shifts(const SDL_Surface *surf, LassShift *sh) {
    const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(surf->format);

    if (!fmt || fmt->bytes_per_pixel != 4 || !fmt->Amask) {
        return 0;
    }
    sh->r = fmt->Rshift;
    sh->g = fmt->Gshift;
    sh->b = fmt->Bshift;
    sh->a = fmt->Ashift;

    return 1;
}

static void lass_blend_image(uint32_t *pixels, int pitch_px, int surf_w,
                             int surf_h, const ASS_Image *img, int ox, int oy,
                             const LassShift *sh) {
    unsigned sr = (img->color >> 24) & 0xFF;
    unsigned sg = (img->color >> 16) & 0xFF;
    unsigned sb = (img->color >> 8) & 0xFF;
    unsigned opacity = 255 - (img->color & 0xFF);

    if (!opacity) {
        return;
    }

    for (int y = 0; y < img->h; y++) {
        int dy = oy + y;
        const unsigned char *src;
        uint32_t *dst;

        if (dy < 0 || dy >= surf_h) {
            continue;
        }
        src = img->bitmap + (size_t)y * (size_t)img->stride;
        dst = pixels + (size_t)dy * (size_t)pitch_px;

        for (int x = 0; x < img->w; x++) {
            unsigned cov = src[x];
            unsigned a, inv;
            uint32_t d;
            unsigned da, dr, dg, db;
            int dx = ox + x;

            if (!cov || dx < 0 || dx >= surf_w) {
                continue;
            }
            a = (cov * opacity + 127) / 255;
            if (!a) {
                continue;
            }
            inv = 255 - a;

            d = dst[dx];
            da = (d >> sh->a) & 0xFF;
            dr = (d >> sh->r) & 0xFF;
            dg = (d >> sh->g) & 0xFF;
            db = (d >> sh->b) & 0xFF;

            dr = (sr * a + 127) / 255 + (dr * inv + 127) / 255;
            dg = (sg * a + 127) / 255 + (dg * inv + 127) / 255;
            db = (sb * a + 127) / 255 + (db * inv + 127) / 255;
            da = a + (da * inv + 127) / 255;

            dst[dx] = (FFMIN(da, 255u) << sh->a) | (FFMIN(dr, 255u) << sh->r) |
                (FFMIN(dg, 255u) << sh->g) | (FFMIN(db, 255u) << sh->b);
        }
    }
}

void lass_blend(SDL_Surface *dst, const ASS_Image *img, int x, int y) {
    LassShift sh;

    if (!dst || !lass_shifts(dst, &sh)) {
        return;
    }

    for (const ASS_Image *p = img; p; p = p->next) {
        if (p->w <= 0 || p->h <= 0) {
            continue;
        }
        lass_blend_image(dst->pixels, dst->pitch / 4, dst->w, dst->h, p,
                         p->dst_x - x, p->dst_y - y, &sh);
    }
}

void lass_blit(SDL_Surface *dst, SDL_Surface *src, int x, int y) {
    LassShift ds, ss;

    if (!dst || !src || !lass_shifts(dst, &ds) || !lass_shifts(src, &ss)) {
        return;
    }

    for (int sy = 0; sy < src->h; sy++) {
        int dy = y + sy;
        const uint32_t *sp;
        uint32_t *dp;

        if (dy < 0 || dy >= dst->h) {
            continue;
        }
        sp = (const uint32_t *)((const unsigned char *)src->pixels +
                                (size_t)sy * (size_t)src->pitch);
        dp = (uint32_t *)((unsigned char *)dst->pixels +
                          (size_t)dy * (size_t)dst->pitch);

        for (int sx = 0; sx < src->w; sx++) {
            uint32_t s = sp[sx];
            unsigned a = (s >> ss.a) & 0xFF;
            unsigned inv = 255 - a;
            uint32_t d;
            unsigned dr, dg, db, da;

            if (!a) {
                continue;
            }
            if (x + sx < 0 || x + sx >= dst->w) {
                continue;
            }
            d = dp[x + sx];
            da = (d >> ds.a) & 0xFF;
            dr = (d >> ds.r) & 0xFF;
            dg = (d >> ds.g) & 0xFF;
            db = (d >> ds.b) & 0xFF;

            dr = ((s >> ss.r) & 0xFF) + (dr * inv + 127) / 255;
            dg = ((s >> ss.g) & 0xFF) + (dg * inv + 127) / 255;
            db = ((s >> ss.b) & 0xFF) + (db * inv + 127) / 255;
            da = a + (da * inv + 127) / 255;

            dp[x + sx] = (FFMIN(da, 255u) << ds.a) | (FFMIN(dr, 255u) << ds.r) |
                (FFMIN(dg, 255u) << ds.g) | (FFMIN(db, 255u) << ds.b);
        }
    }
}

size_t lass_escape(char *out, size_t outsz, const char *text) {
    size_t o = 0, boundary = 0;
    int run = 0;
    int start_of_line = 1;

    if (!out || outsz < 2) {
        return 0;
    }

    /* clang-format off */
#define LASS_PUT(str, n)             \
    do {                             \
        if (o + (n) >= outsz) {      \
            out[boundary] = '\0';    \
            return boundary;         \
        }                            \
        memcpy(out + o, (str), (n)); \
        o += (n);                    \
    } while (0)
    /* clang-format on */

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        int lead = (c & 0xC0) != 0x80;

        if (lead) {
            boundary = o;
        }

        if (lead && run >= LASS_BREAK_RUN) {
            LASS_PUT("\xE2\x80\x8B", 3);
            run = 0;
        }

        if (c == '\n' || c == '\r') {
            if (c == '\r' && p[1] == '\n') {
                continue;
            }
            LASS_PUT("\\N", 2);
            start_of_line = 1;
            run = 0;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (start_of_line) {
                LASS_PUT("\\h", 2);
            } else {
                LASS_PUT(" ", 1);
            }
            run = 0;
            continue;
        }

        start_of_line = 0;

        if (c == '{' || c == '}') {
            char esc[2] = {'\\', (char)c};

            LASS_PUT(esc, 2);
        } else if (c == '\\') {
            LASS_PUT("\\\xE2\x81\xA0", 4);
        } else {
            LASS_PUT(p, 1);
        }

        run += lead;
    }

#undef LASS_PUT

    out[o] = '\0';

    return o;
}

static uint32_t lass_utf8_next(const char *s, size_t len, size_t *pos) {
    unsigned char c = (unsigned char)s[*pos];
    uint32_t cp;
    int extra;

    if (c < 0x80) {
        (*pos)++;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        extra = 3;
    } else {
        (*pos)++;
        return 0xFFFD;
    }
    if (*pos + (size_t)extra >= len) {
        *pos = len;
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; k++) {
        unsigned char cc = (unsigned char)s[*pos + k];

        if ((cc & 0xC0) != 0x80) {
            (*pos)++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *pos += (size_t)extra + 1;

    return cp;
}

static int lass_is_escaped(char c) {
    return c == 'N' || c == 'n' || c == 'h' || c == '{' || c == '}';
}

static int cp_is_emoji_base(uint32_t cp) {
    return cp >= 0x1F000 && cp <= 0x1FAFF;
}

static int cp_is_emoji_join(uint32_t cp) {
    return cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 ||
        (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

int lass_route_emoji(const char *in, const char *font, LassStyleFont lookup,
                     void *ctx, char *out, size_t outsz) {
    const char *cur_font = font;
    size_t o = 0, p, len = strlen(in);
    char held[256];
    int in_override = 0;
    int found = 0;

    if (!font || !*font) {
        return 0;
    }

    for (p = 0; p < len;) {
        size_t save = p;
        uint32_t cp = lass_utf8_next(in, len, &p);

        if (!in_override && cp == '\\' && p < len && lass_is_escaped(in[p])) {
            p++;
        } else if (cp == '{') {
            in_override = 1;
        } else if (cp == '}') {
            in_override = 0;
        } else if (!in_override && cp_is_emoji_base(cp)) {
            found = 1;
            break;
        }
        if (p == save) {
            break;
        }
    }
    if (!found) {
        return 0;
    }

#define LASS_EMIT(str, n) \
    do { \
        if (o + (n) >= outsz) { \
            return 0; \
        } \
        memcpy(out + o, (str), (n)); \
        o += (n); \
    } while (0)

    in_override = 0;
    for (p = 0; p < len;) {
        size_t start = p;
        uint32_t cp = lass_utf8_next(in, len, &p);

        if (p == start) {
            break;
        }

        if (in_override) {
            if (cp == '\\' && p < len) {
                if (!strncmp(in + p, "fn", 2)) {
                    const char *fn = in + p + 2;
                    size_t n = strcspn(fn, "\\}");

                    if (n && n < sizeof(held)) {
                        memcpy(held, fn, n);
                        held[n] = '\0';
                        cur_font = held;
                    }
                } else if (in[p] == 'r') {
                    const char *rn = in + p + 1;
                    size_t n = strcspn(rn, "\\}");
                    const char *f = (n && lookup) ? lookup(rn, n, ctx) : font;

                    if (f && *f) {
                        cur_font = f;
                    }
                }
            }
            if (cp == '}') {
                in_override = 0;
            }
            LASS_EMIT(in + start, p - start);
            continue;
        }

        if (cp == '\\' && p < len && lass_is_escaped(in[p])) {
            p++;
            LASS_EMIT(in + start, p - start);
            continue;
        }

        if (cp == '{') {
            in_override = 1;
            LASS_EMIT(in + start, p - start);
            continue;
        }

        if (cp_is_emoji_base(cp)) {
            LASS_EMIT("{\\fn" LASS_EMOJI_FAMILY "}",
                      sizeof("{\\fn" LASS_EMOJI_FAMILY "}") - 1);
            for (;;) {
                size_t save = p;
                uint32_t n;

                if (p >= len) {
                    break;
                }
                n = lass_utf8_next(in, len, &p);
                if (cp_is_emoji_base(n) || cp_is_emoji_join(n)) {
                    continue;
                }
                p = save;
                break;
            }
            LASS_EMIT(in + start, p - start);
            LASS_EMIT("{\\fn", 4);
            LASS_EMIT(cur_font, strlen(cur_font));
            LASS_EMIT("}", 1);
            continue;
        }

        if (p < len) {
            size_t peek = p;

            if (lass_utf8_next(in, len, &peek) == 0xFE0F) {
                LASS_EMIT("{\\fn" LASS_EMOJI_FAMILY "}",
                          sizeof("{\\fn" LASS_EMOJI_FAMILY "}") - 1);
                LASS_EMIT(in + start, peek - start);
                LASS_EMIT("{\\fn", 4);
                LASS_EMIT(cur_font, strlen(cur_font));
                LASS_EMIT("}", 1);
                p = peek;
                continue;
            }
        }

        LASS_EMIT(in + start, p - start);
    }

    if (o >= outsz) {
        return 0;
    }
    out[o] = '\0';

#undef LASS_EMIT

    return 1;
}
