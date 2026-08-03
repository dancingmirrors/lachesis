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

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <libavutil/attributes.h>
#include <libavutil/avutil.h>
#include <libavutil/macros.h>

#include "lachesis_osd.h"
#include "lachesis_osd_font.h"
#include "lachesis_osd_ui_font.h"
#include "lachesis_subtitles.h"

#define OSD_STATUS_DURATION_MS 1000
#define OSD_SEEK_DURATION_MS 1000
#define OSD_MESSAGE_DURATION_MS 3000
#define OSD_INFO_MAX_LINES 512
#define OSD_INFO_TEXT_MAX 8192
#define OSD_WRAP_W_MAX (1 << 20)
#define OSD_FIT_PASSES 8

#define OSD_FONT_REF_720 55.0
#define OSD_OUTLINE_RATIO (2.0 / 55.0)
#define OSD_INFO_SIZE_RATIO 0.70
#define OSD_SUPERSAMPLE 2
#define OSD_FONT_INIT_PX 55
#define OSD_MAX_FALLBACK_FONTS 8
#define OSD_SUB_RESERVE_RATIO 0.09

typedef enum { OSD_FACE_UI = 0,
               OSD_FACE_SYM,
               OSD_FACE_COUNT } OsdFace;

typedef struct {
    TTF_Font *fill;
    TTF_Font *outline;
} OsdFaceSet;

static OsdFaceSet osd_faces[OSD_FACE_COUNT];
static int osd_base_px = 0;
static int osd_outline_px = 0;
static int osd_ss = 1;

static char osd_ui_font_path[512];
static TTF_Font *osd_fallback_fonts[OSD_MAX_FALLBACK_FONTS];
static int osd_num_fallback_fonts = 0;

static SDL_Surface *osd_surface = NULL;
static SDL_Renderer *osd_sw_renderer = NULL;

static int64_t osd_status_show_until = 0;
static int64_t osd_volume_show_until = 0;
static char osd_message[1024];
static int64_t osd_message_show_until = 0;
static int osd_info_sticky = 0;
static int osd_info_page = 1;
static char osd_delete_prompt[1024];
static int osd_delete_prompt_active = 0;
static OsdInfoProvider osd_info_provider = NULL;
static OsdInfoProvider osd_stats_provider = NULL;

typedef struct {
    SDL_Surface *surf;
    char text[OSD_INFO_TEXT_MAX];
    int box_w, box_h;
    int base_px, ss;
    int pad;
} OsdInfoCache;
static OsdInfoCache osd_info_cache;

typedef enum { OSD_SHRINK_NONE = 0,
               OSD_SHRINK_FIT } OsdShrink;

typedef struct {
    OsdFace face;
    double ratio;
    unsigned mono : 1;
    OsdShrink shrink : 2;
} OsdTextStyle;

static const OsdTextStyle ST_PRIMARY = {OSD_FACE_UI, 1.00, 1, OSD_SHRINK_NONE};
static const OsdTextStyle ST_MESSAGE = {OSD_FACE_UI, 1.00, 1, OSD_SHRINK_FIT};
static const OsdTextStyle ST_MODAL = {OSD_FACE_UI, 1.00, 0, OSD_SHRINK_FIT};
static const OsdTextStyle ST_INFO = {OSD_FACE_UI, OSD_INFO_SIZE_RATIO, 1, OSD_SHRINK_FIT};
static const OsdTextStyle ST_SYMBOL = {OSD_FACE_SYM, 1.00, 0, OSD_SHRINK_NONE};

typedef struct {
    int x, y, w, h;
} OsdRect;

typedef struct {
    const char *p;
    size_t len;
} OsdSpan;

typedef struct {
    int cw, ch;
    int mx, my;
    int stack_gap;
    int line_gap;
    int row_gap;
    OsdRect topleft;
    int topleft_cur;
    int sub_bottom;
    int subs_top;
    int seek_bar_y;
    int seek_label_h;
} OsdLayout;

void format_time(char *buf, int bufsz, double secs) {
    int s = (int)secs;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sc = s % 60;
    snprintf(buf, bufsz, "%02d:%02d:%02d", h, m, sc);
}

static int osd_px_scale(int canvas_h, double at720) {
    int v = (int)(canvas_h * at720 / 720.0 + 0.5);
    return v;
}

static void osd_face_setup(TTF_Font *f, int outline) {
    if (!f) {
        return;
    }
    TTF_SetFontHinting(f, TTF_HINTING_NONE);
    if (outline > 0) {
#ifdef TTF_PROP_FONT_OUTLINE_LINE_JOIN_NUMBER
        SDL_PropertiesID props = TTF_GetFontProperties(f);
        if (props) {
            SDL_SetNumberProperty(props, TTF_PROP_FONT_OUTLINE_LINE_JOIN_NUMBER, 0);
            SDL_SetNumberProperty(props, TTF_PROP_FONT_OUTLINE_LINE_CAP_NUMBER, 1);
        }
#endif
        TTF_SetFontOutline(f, outline);
    }
}

static TTF_Font *osd_open_sym(float px) {
    SDL_IOStream *rw = SDL_IOFromConstMem(osd_font_data, (size_t)osd_font_size);
    if (!rw) {
        return NULL;
    }
    return TTF_OpenFontIO(rw, true, px);
}

const void *osd_embedded_ui_font(size_t *size) {
    if (size) {
        *size = (size_t)osd_ui_font_data_size;
    }
    return osd_ui_font_data;
}

static TTF_Font *osd_open_ui(float px) {
    if (osd_ui_font_path[0]) {
        TTF_Font *f = TTF_OpenFont(osd_ui_font_path, px);
        if (f) {
            return f;
        }
    }
    SDL_IOStream *rw =
        SDL_IOFromConstMem(osd_ui_font_data, (size_t)osd_ui_font_data_size);
    if (!rw) {
        return NULL;
    }
    return TTF_OpenFontIO(rw, true, px);
}

static void osd_invalidate_measurements(void);

static void osd_faces_ensure(int canvas_h) {
    if (canvas_h <= 0 || !osd_faces[OSD_FACE_UI].fill) {
        return;
    }
    int px = osd_px_scale(canvas_h, OSD_FONT_REF_720);
    if (px < 12) {
        px = 12;
    }
    if (px > 240) {
        px = 240;
    }
    if (px == osd_base_px) {
        return;
    }
    osd_base_px = px;
    osd_ss = OSD_SUPERSAMPLE > 0 ? OSD_SUPERSAMPLE : 1;
    osd_invalidate_textures();
    osd_invalidate_measurements();
    osd_outline_px = (int)(px * OSD_OUTLINE_RATIO + 0.5);
    if (osd_outline_px < 1) {
        osd_outline_px = 1;
    }
    float fpx = (float)(px * osd_ss);
    int fol = osd_outline_px * osd_ss;
    for (int i = 0; i < OSD_FACE_COUNT; i++) {
        if (osd_faces[i].fill) {
            TTF_SetFontSize(osd_faces[i].fill, fpx);
        }
        if (osd_faces[i].outline) {
            TTF_SetFontSize(osd_faces[i].outline, fpx);
            TTF_SetFontOutline(osd_faces[i].outline, fol);
        }
    }
    for (int i = 0; i < osd_num_fallback_fonts; i++) {
        if (osd_fallback_fonts[i]) {
            TTF_SetFontSize(osd_fallback_fonts[i], fpx);
        }
    }
}

#define OSD_RUN_MAX 11
#define OSD_MEASURE_CACHE_SIZE 128
#define OSD_GLYPH_CACHE_SIZE 128

typedef struct {
    char text[OSD_RUN_MAX + 1];
    size_t len;
    const void *font;
    int base_px, ss;
    int w, h;
    unsigned valid : 1;
} OsdMeasureEntry;

typedef struct {
    char text[OSD_RUN_MAX + 1];
    const void *faces;
    SDL_Renderer *renderer;
    SDL_Color fg;
    int base_px, ss;
    SDL_Texture *fill;
    SDL_Texture *outline;
    float fill_w, fill_h;
    float outline_w, outline_h;
    unsigned valid : 1;
} OsdGlyphEntry;

static OsdMeasureEntry osd_measure_cache[OSD_MEASURE_CACHE_SIZE];
static OsdGlyphEntry osd_glyph_cache[OSD_GLYPH_CACHE_SIZE];

static unsigned osd_cache_hash(const char *text, size_t len, const void *ptr) {
    unsigned h = 2166136261u;

    for (size_t i = 0; i < len; i++) {
        h = (h ^ (unsigned char)text[i]) * 16777619u;
    }
    for (size_t i = 0; i < sizeof(ptr); i++) {
        h = (h ^ (unsigned)(((uintptr_t)ptr >> (i * 8)) & 0xff)) * 16777619u;
    }

    return h;
}

static void osd_invalidate_measurements(void) {
    memset(osd_measure_cache, 0, sizeof(osd_measure_cache));
}

static void osd_invalidate_glyphs(void) {
    for (int i = 0; i < OSD_GLYPH_CACHE_SIZE; i++) {
        OsdGlyphEntry *e = &osd_glyph_cache[i];

        SDL_DestroyTexture(e->fill);
        SDL_DestroyTexture(e->outline);
        e->fill = NULL;
        e->outline = NULL;
        e->valid = 0;
    }
}

static void osd_ui_measure(TTF_Font *font, const char *text, size_t len,
                           int *w, int *h) {
    int tw = 0, th = 0;
    int ss = osd_ss > 0 ? osd_ss : 1;
    OsdMeasureEntry *e = NULL;

    if (text && !len) {
        len = strlen(text);
    }

    if (font && len > 0 && len <= OSD_RUN_MAX) {
        e = &osd_measure_cache[osd_cache_hash(text, len, font) %
                               OSD_MEASURE_CACHE_SIZE];
        if (e->valid && e->font == font && e->base_px == osd_base_px &&
            e->ss == ss && e->len == len && !memcmp(e->text, text, len)) {
            if (w) {
                *w = e->w;
            }
            if (h) {
                *h = e->h;
            }
            return;
        }
    }

    if (font && text && len) {
        TTF_GetStringSize(font, text, len, &tw, &th);
    }
    tw /= ss;
    th /= ss;

    if (e) {
        memcpy(e->text, text, len);
        e->text[len] = '\0';
        e->len = len;
        e->font = font;
        e->base_px = osd_base_px;
        e->ss = ss;
        e->w = tw;
        e->h = th;
        e->valid = 1;
    }

    if (w) {
        *w = tw;
    }
    if (h) {
        *h = th;
    }
}

static void osd_blit_tex(SDL_Renderer *r, SDL_Texture *t, float w, float h,
                         float x, float y, double scale, SDL_BlendMode blend) {
    SDL_SetTextureBlendMode(t, blend);
    SDL_SetTextureScaleMode(t, scale == 1.0 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_FRect d = {x, y, (float)(w * scale), (float)(h * scale)};
    SDL_RenderTexture(r, t, NULL, &d);
}

static void osd_draw_tex(SDL_Renderer *r, SDL_Surface *s, float x, float y,
                         double scale, SDL_BlendMode blend) {
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (!t) {
        return;
    }
    osd_blit_tex(r, t, (float)s->w, (float)s->h, x, y, scale, blend);
    SDL_DestroyTexture(t);
}

typedef struct {
    SDL_Texture *tex;
    SDL_Renderer *renderer;
    SDL_Surface *surf;
} OsdCachedTexture;

static void osd_cached_texture_clear(OsdCachedTexture *ct) {
    SDL_DestroyTexture(ct->tex);
    ct->tex = NULL;
    ct->renderer = NULL;
    ct->surf = NULL;
}

static void osd_draw_cached_surface(SDL_Renderer *r, OsdCachedTexture *ct,
                                    SDL_Surface *s, float x, float y,
                                    SDL_BlendMode blend) {
    if (!r || !s) {
        return;
    }
    if (!ct->tex || ct->renderer != r || ct->surf != s) {
        osd_cached_texture_clear(ct);
        ct->tex = SDL_CreateTextureFromSurface(r, s);
        if (!ct->tex) {
            return;
        }
        ct->renderer = r;
        ct->surf = s;
    }
    osd_blit_tex(r, ct->tex, (float)s->w, (float)s->h, x, y, 1.0, blend);
}

static OsdCachedTexture osd_info_tex;
static OsdCachedTexture osd_sub_tex;

static unsigned osd_sub_generation;

void osd_invalidate_textures(void) {
    osd_cached_texture_clear(&osd_info_tex);
    osd_cached_texture_clear(&osd_sub_tex);
    osd_invalidate_glyphs();
}

static int osd_utf8_next(const char *text, size_t len, size_t *pos);

static int osd_is_tabular(const char *p) {
    return *p >= '0' && *p <= '9';
}

static SDL_Texture *osd_texture_from_surface(SDL_Renderer *r, SDL_Surface *s,
                                             float *w, float *h) {
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(r, s) : NULL;

    if (t) {
        *w = (float)s->w;
        *h = (float)s->h;
    }

    return t;
}

static int osd_renderer_is_persistent(SDL_Renderer *r) {
    return r && (r == renderer || r == osd_sw_renderer);
}

static OsdGlyphEntry *osd_cache_run(SDL_Renderer *r, OsdFaceSet *fs,
                                    const char *text, size_t len, SDL_Color fg) {
    SDL_Color black = {0, 0, 0, 255};
    int ss = osd_ss > 0 ? osd_ss : 1;
    OsdGlyphEntry *e = &osd_glyph_cache[osd_cache_hash(text, len, fs) %
                                        OSD_GLYPH_CACHE_SIZE];

    if (e->valid && e->faces == fs && e->renderer == r &&
        e->base_px == osd_base_px && e->ss == ss &&
        !memcmp(&e->fg, &fg, sizeof(fg)) && e->text[len] == '\0' &&
        !memcmp(e->text, text, len)) {
        return e;
    }

    SDL_DestroyTexture(e->fill);
    SDL_DestroyTexture(e->outline);
    memset(e, 0, sizeof(*e));

    SDL_Surface *ft = TTF_RenderText_Blended(fs->fill, text, len, fg);
    e->fill = osd_texture_from_surface(r, ft, &e->fill_w, &e->fill_h);
    SDL_DestroySurface(ft);
    if (!e->fill) {
        return NULL;
    }
    if (fs->outline) {
        SDL_Surface *os = TTF_RenderText_Blended(fs->outline, text, len, black);
        e->outline = osd_texture_from_surface(r, os, &e->outline_w, &e->outline_h);
        SDL_DestroySurface(os);
    }

    memcpy(e->text, text, len);
    e->text[len] = '\0';
    e->faces = fs;
    e->renderer = r;
    e->fg = fg;
    e->base_px = osd_base_px;
    e->ss = ss;
    e->valid = 1;

    return e;
}

static void osd_blit_run(SDL_Renderer *r, OsdFaceSet *fs, const char *text,
                         size_t len, int x, int y, SDL_Color fg, double fscale) {
    SDL_Color black = {0, 0, 0, 255};
    double off = (double)osd_outline_px * osd_ss * fscale;
    size_t run_len = len ? len : strlen(text);

    if (run_len <= OSD_RUN_MAX && osd_renderer_is_persistent(r)) {
        OsdGlyphEntry *e = osd_cache_run(r, fs, text, run_len, fg);

        if (!e) {
            return;
        }
        if (e->outline) {
            osd_blit_tex(r, e->outline, e->outline_w, e->outline_h,
                         (float)(x - off), (float)(y - off), fscale,
                         SDL_BLENDMODE_BLEND);
        }
        osd_blit_tex(r, e->fill, e->fill_w, e->fill_h, (float)x, (float)y,
                     fscale, SDL_BLENDMODE_BLEND);
        return;
    }

    SDL_Surface *os =
        fs->outline ? TTF_RenderText_Blended(fs->outline, text, len, black) : NULL;
    SDL_Surface *ft = TTF_RenderText_Blended(fs->fill, text, len, fg);
    if (!ft) {
        SDL_DestroySurface(os);
        return;
    }
    if (os) {
        osd_draw_tex(r, os, (float)(x - off), (float)(y - off), fscale,
                     SDL_BLENDMODE_BLEND);
        SDL_DestroySurface(os);
    }
    osd_draw_tex(r, ft, (float)x, (float)y, fscale, SDL_BLENDMODE_BLEND);
    SDL_DestroySurface(ft);
}

static int osd_line(SDL_Renderer *r, OsdFace face, int mono, const char *text,
                    size_t len, int x, int y, SDL_Color fg, double s) {
    OsdFaceSet *fs = &osd_faces[face];
    if (!fs->fill || !text) {
        return 0;
    }
    if (!len) {
        len = strlen(text);
    }
    if (!len) {
        return 0;
    }
    int ss = osd_ss > 0 ? osd_ss : 1;
    double fscale = s / ss;

    if (!mono) {
        int w = 0;
        osd_ui_measure(fs->fill, text, len, &w, NULL);
        if (r) {
            osd_blit_run(r, fs, text, len, x, y, fg, fscale);
        }
        return (int)(w * s + 0.5);
    }

    int cell_w = 0;
    osd_ui_measure(fs->fill, "0", 1, &cell_w, NULL);
    if (cell_w <= 0) {
        int w = 0;
        osd_ui_measure(fs->fill, text, len, &w, NULL);
        if (r) {
            osd_blit_run(r, fs, text, len, x, y, fg, fscale);
        }
        return (int)(w * s + 0.5);
    }

    double cell = cell_w * s;
    double cx = x;
    size_t pos = 0;
    char g[8];
    while (pos < len) {
        size_t st = pos;
        osd_utf8_next(text, len, &pos);
        size_t clen = pos - st;
        if (clen >= sizeof(g)) {
            clen = sizeof(g) - 1;
        }
        memcpy(g, text + st, clen);
        g[clen] = '\0';
        int gw = 0;
        osd_ui_measure(fs->fill, g, clen, &gw, NULL);
        double adv = gw * s;
        int tab = (clen == 1) && osd_is_tabular(text + st);
        if (tab) {
            double gx = cx + (cell - adv) / 2.0;
            if (r) {
                osd_blit_run(r, fs, g, clen, (int)(gx + 0.5), y, fg, fscale);
            }
            cx += cell;
        } else {
            if (r) {
                osd_blit_run(r, fs, g, clen, (int)(cx + 0.5), y, fg, fscale);
            }
            cx += adv;
        }
    }
    return (int)(cx - x + 0.5);
}

static int osd_line_width(const OsdTextStyle *st, const char *text) {
    SDL_Color fg = {255, 255, 255, 255};
    return osd_line(NULL, st->face, st->mono, text, 0, 0, 0, fg, st->ratio);
}

static int osd_run_width(const OsdTextStyle *st, const char *text, size_t len) {
    SDL_Color fg = {255, 255, 255, 255};
    return osd_line(NULL, st->face, st->mono, text, len, 0, 0, fg, 1.0);
}

static int osd_line_height(const OsdTextStyle *st, const char *text) {
    OsdFaceSet *fs = &osd_faces[st->face];
    if (!fs->fill) {
        return 0;
    }
    int h = 0;
    osd_ui_measure(fs->fill, text && text[0] ? text : "0", 0, NULL, &h);
    return (int)(h * st->ratio + 0.5);
}

static double osd_fit_w(int natural_w, int max_w) {
    if (natural_w <= 0 || max_w <= 0 || natural_w <= max_w) {
        return 1.0;
    }
    return (double)max_w / (double)natural_w;
}

static int osd_wrap_w(int box_w, double s) {
    double w = s > 0.0 ? (double)box_w / s : (double)box_w;

    if (w < 1.0) {
        return 1;
    }
    if (w > OSD_WRAP_W_MAX) {
        return OSD_WRAP_W_MAX;
    }

    return (int)(w + 0.5);
}

static double osd_block_h(int nline, double nlh, double s, double gap_unit) {
    if (nline < 1) {
        return 0.0;
    }
    return s * (nline * nlh + gap_unit * (nline - 1));
}

static int osd_wrap_into(const OsdTextStyle *st, const char *src, size_t srclen,
                         int max_w, OsdSpan *lines, int nline, int cap) {
    if (max_w < 1) {
        max_w = 1;
    }

    const char *cur = NULL;
    size_t cl = 0;
    int cur_w = 0;

    const char *p = src;
    const char *end = src + srclen;
    while (p < end && nline < cap) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
            p++;
        }
        if (p >= end) {
            break;
        }
        const char *ws = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r') {
            p++;
        }
        size_t wlen = (size_t)(p - ws);
        int ww = osd_run_width(st, ws, wlen);

        if (cl > 0) {
            size_t gap_len = (size_t)(ws - (cur + cl));
            int gap_w = gap_len ? osd_run_width(st, cur + cl, gap_len) : 0;

            if (cur_w + gap_w + ww <= max_w) {
                cl = (size_t)(ws + wlen - cur);
                cur_w += gap_w + ww;
                continue;
            }
            lines[nline].p = cur;
            lines[nline].len = cl;
            nline++;
            cur = NULL;
            cl = 0;
            cur_w = 0;
            if (nline >= cap) {
                break;
            }
        }

        if (ww <= max_w) {
            cur = ws;
            cl = wlen;
            cur_w = ww;
            continue;
        }

        size_t i = 0;
        const char *pp = ws;
        size_t pl = 0;
        int pw = 0;
        while (i < wlen && nline < cap) {
            size_t gs = i;
            osd_utf8_next(ws, wlen, &i);
            size_t cand = i - (size_t)(pp - ws);
            int cand_w = st->mono
                ? pw + osd_run_width(st, ws + gs, i - gs)
                : osd_run_width(st, pp, cand);

            if (pl > 0 && cand_w > max_w) {
                lines[nline].p = pp;
                lines[nline].len = pl;
                nline++;
                pp = ws + gs;
                pl = i - gs;
                pw = osd_run_width(st, pp, pl);
                continue;
            }
            pl = cand;
            pw = cand_w;
        }
        if (pl > 0 && nline < cap) {
            cur = pp;
            cl = pl;
            cur_w = pw;
        }
    }
    if (cl > 0 && nline < cap) {
        lines[nline].p = cur;
        lines[nline].len = cl;
        nline++;
    }

    return nline;
}

static int osd_wrap_block(const OsdTextStyle *st, const char *text, int max_w,
                          OsdSpan *lines, int cap) {
    const char *p = text;
    int nline = 0;

    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);

        if (nline >= cap) {
            break;
        }
        if (!seglen) {
            lines[nline].p = p;
            lines[nline].len = 0;
            nline++;
        } else {
            nline = osd_wrap_into(st, p, seglen, max_w, lines, nline, cap);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    return nline;
}

static int osd_text_block(SDL_Renderer *r, const OsdTextStyle *st,
                          const char *text, OsdRect box, int line_gap,
                          SDL_Color fg) {
    OsdFaceSet *fs = &osd_faces[st->face];
    if (!fs->fill || !text || !text[0] || box.w < 1 || box.h < 1 ||
        st->ratio <= 0.0) {
        return 0;
    }
    int ss = osd_ss > 0 ? osd_ss : 1;
    int fh = TTF_GetFontHeight(fs->fill);
    double nlh = fh > 0 ? (double)fh / ss : 1.0;
    if (nlh < 1.0) {
        nlh = 1.0;
    }
    double ratio = st->ratio;
    double gap_unit = (double)line_gap / ratio;

    OsdSpan lines[OSD_INFO_MAX_LINES];
    double s = ratio;
    int nline = osd_wrap_block(st, text, osd_wrap_w(box.w, s), lines,
                               OSD_INFO_MAX_LINES);
    if (nline == 0) {
        return 0;
    }

    if (st->shrink == OSD_SHRINK_FIT &&
        osd_block_h(nline, nlh, s, gap_unit) > box.h) {
        double denom = nline * nlh + gap_unit * (nline - 1);
        double lo = denom > 0.0 ? box.h / denom : s;
        double hi = s;

        if (lo > hi) {
            lo = hi;
        }
        for (int pass = 0; pass < OSD_FIT_PASSES; pass++) {
            double mid = 0.5 * (lo + hi);
            int n = osd_wrap_block(st, text, osd_wrap_w(box.w, mid),
                                   lines, OSD_INFO_MAX_LINES);
            if (n > 0 && osd_block_h(n, nlh, mid, gap_unit) <= box.h) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        s = lo;
        nline = osd_wrap_block(st, text, osd_wrap_w(box.w, s), lines,
                               OSD_INFO_MAX_LINES);
        if (nline == 0) {
            return 0;
        }

        if (osd_block_h(nline, nlh, s, gap_unit) > box.h) {
            denom = nline * nlh + gap_unit * (nline - 1);
            if (denom > 0.0) {
                s = box.h / denom;
                nline = osd_wrap_block(st, text, osd_wrap_w(box.w, s), lines,
                                       OSD_INFO_MAX_LINES);
                if (nline == 0) {
                    return 0;
                }
            }
        }
    }

    double line_h = nlh * s;
    double step = line_h + gap_unit * s;
    int ndraw = nline;
    if (st->shrink == OSD_SHRINK_FIT && step > 0.0) {
        int fitn = (int)((box.h - line_h) / step + 1.0 + 1e-3);
        if (fitn < 1) {
            fitn = 1;
        }
        if (fitn < ndraw) {
            ndraw = fitn;
        }
    }

    double y = box.y;
    for (int i = 0; i < ndraw; i++) {
        if (lines[i].len) {
            osd_line(r, st->face, st->mono, lines[i].p, lines[i].len, box.x,
                     (int)(y + 0.5), fg, s);
        }
        y += step;
    }

    return (int)((ndraw - 1) * step + line_h + 0.5);
}

static double osd_display_pos(VideoState *is) {
    return effective_playhead(is);
}

static int has_active_subtitle(VideoState *is) {
    if (!is->subtitle_st) {
        return 0;
    }
    return subtitles_visible_at(get_master_clock(is));
}

static int osd_utf8_next(const char *text, size_t len, size_t *pos) {
    unsigned char c = (unsigned char)text[*pos];
    uint32_t cp;
    int extra;
    if (c < 0x80) {
        cp = c;
        extra = 0;
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
        unsigned char cc = (unsigned char)text[*pos + k];
        if ((cc & 0xC0) != 0x80) {
            (*pos)++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *pos += (size_t)extra + 1;
    return cp;
}

static void osd_draw_subtitle(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    SubtitleOverlay ov;
    SDL_Rect vr = is->render_params.target_rect;

    if (!subtitles_render(is, L->cw, L->ch, &vr, get_master_clock(is), &ov) ||
        !ov.surf) {
        return;
    }

    if (ov.generation != osd_sub_generation) {
        osd_cached_texture_clear(&osd_sub_tex);
        osd_sub_generation = ov.generation;
    }

    L->subs_top = ov.y;

    osd_draw_cached_surface(r, &osd_sub_tex, ov.surf, (float)ov.x, (float)ov.y,
                            SDL_BLENDMODE_BLEND_PREMULTIPLIED);
}

static void osd_layout_init(OsdLayout *L, int cw, int ch) {
    L->cw = cw;
    L->ch = ch;
    L->mx = osd_px_scale(ch, 25.0);
    L->my = osd_px_scale(ch, 22.0);
    L->stack_gap = osd_px_scale(ch, 10.0);
    L->line_gap = osd_px_scale(ch, 8.0);
    L->row_gap = osd_px_scale(ch, 12.0);
    L->topleft.x = L->mx;
    L->topleft.y = L->my;
    L->topleft.w = cw - 2 * L->mx;
    L->topleft.h = ch - 2 * L->my;
    L->topleft_cur = L->my;
    L->sub_bottom = ch / 12;
    L->subs_top = ch;
    L->seek_bar_y = ch * 3 / 4;
    L->seek_label_h = osd_line_height(&ST_PRIMARY, "00:00:00");
}

static OsdRect osd_stack_remaining(const OsdLayout *L, int floor_y) {
    OsdRect b = {L->mx, L->topleft_cur, L->topleft.w,
                 FFMAX(1, floor_y - L->topleft_cur)};
    return b;
}

static void osd_stack_advance(OsdLayout *L, int consumed_h) {
    L->topleft_cur += consumed_h + L->stack_gap;
}

static void osd_draw_abloop(SDL_Renderer *r, OsdLayout *L) {
    char a_str[16], line[64];
    format_time(a_str, sizeof(a_str), ab_loop_a < 0 ? 0.0 : ab_loop_a);
    snprintf(line, sizeof(line), "A-B loop  %s -> [set B]", a_str);
    SDL_Color fg = {255, 255, 255, 255};
    OsdRect b = osd_stack_remaining(L, L->ch - L->my);
    double fit = osd_fit_w(osd_line_width(&ST_PRIMARY, line), b.w);
    int th = (int)(osd_line_height(&ST_PRIMARY, line) * fit + 0.5);
    osd_line(r, ST_PRIMARY.face, ST_PRIMARY.mono, line, 0, b.x, b.y, fg,
             ST_PRIMARY.ratio * fit);
    osd_stack_advance(L, th);
}

static void osd_draw_message(SDL_Renderer *r, OsdLayout *L) {
    SDL_Color fg = {255, 255, 255, 255};
    OsdRect b = osd_stack_remaining(L, L->ch - L->my);
    int consumed = osd_text_block(r, &ST_MESSAGE, osd_message, b, L->line_gap, fg);
    osd_stack_advance(L, consumed);
}

static void osd_draw_status(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    double pos = osd_display_pos(is);
    double dur = (is->ic && is->ic->duration != AV_NOPTS_VALUE)
        ? (double)is->ic->duration / AV_TIME_BASE
        : 0.0;
    if (dur > 0 && !isnan(pos) && pos > dur) {
        pos = dur;
    }
    char pos_str[16], dur_str[16], line[64];
    format_time(pos_str, sizeof(pos_str), isnan(pos) || pos < 0 ? 0.0 : pos);
    format_time(dur_str, sizeof(dur_str), dur);
    int pct = (dur > 0 && !isnan(pos) && pos >= 0)
        ? (int)(100.0 * pos / dur + 0.5)
        : 0;
    const char *sym = is->paused ? "\xEE\x80\x82" : "\xEE\x80\x81";
    snprintf(line, sizeof(line), "%s / %s (%d%%)", pos_str, dur_str, pct);

    SDL_Color fg = {255, 255, 255, 255};
    int have_sym = (osd_faces[OSD_FACE_SYM].fill != NULL);
    int sw = have_sym ? osd_line_width(&ST_SYMBOL, sym) : 0;
    int sh = have_sym ? osd_line_height(&ST_SYMBOL, sym) : 0;
    int tw = osd_line_width(&ST_PRIMARY, line);
    int th = osd_line_height(&ST_PRIMARY, line);
    int gap = have_sym ? L->row_gap : 0;
    OsdRect b = osd_stack_remaining(L, L->ch - L->my);

    double fit = osd_fit_w(sw + gap + tw, b.w);
    sw = (int)(sw * fit + 0.5);
    sh = (int)(sh * fit + 0.5);
    th = (int)(th * fit + 0.5);
    gap = (int)(gap * fit + 0.5);
    int row_h = FFMAX(sh, th);
    if (have_sym) {
        osd_line(r, ST_SYMBOL.face, ST_SYMBOL.mono, sym, 0, b.x,
                 b.y + (row_h - sh) / 2, fg, ST_SYMBOL.ratio * fit);
    }
    osd_line(r, ST_PRIMARY.face, ST_PRIMARY.mono, line, 0, b.x + sw + gap,
             b.y + (row_h - th) / 2, fg, ST_PRIMARY.ratio * fit);

    int consumed = row_h;
    if (dur > 0) {
        double frac = (!isnan(pos) && pos >= 0) ? FFMIN(pos / dur, 1.0) : 0.0;
        int twf = (int)(tw * fit + 0.5);
        int bar_w = sw + gap + twf;
        int bar_h = osd_px_scale(L->ch, 6.0);
        if (bar_h < 3) {
            bar_h = 3;
        }
        int pad = FFMAX(1, bar_h / 3);
        int bar_x = b.x;
        int bar_y = b.y + row_h + L->line_gap + pad;
        SDL_Color bar_bg = {80, 80, 80, 200};
        SDL_Color bar_fg = {210, 210, 210, 255};

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
        SDL_FRect bg_rect = {bar_x - pad, bar_y - pad, bar_w + 2 * pad,
                             bar_h + 2 * pad};
        SDL_RenderFillRect(r, &bg_rect);
        int filled = (int)(bar_w * frac + 0.5);
        if (filled > 0) {
            SDL_SetRenderDrawColor(r, bar_fg.r, bar_fg.g, bar_fg.b, bar_fg.a);
            SDL_FRect fg_rect = {bar_x, bar_y, filled, bar_h};
            SDL_RenderFillRect(r, &fg_rect);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        consumed = row_h + L->line_gap + pad + bar_h + pad;
    }
    osd_stack_advance(L, consumed);
}

static int osd_sub_reserved_top(const OsdLayout *L) {
    int top = L->ch - (int)(L->ch * OSD_SUB_RESERVE_RATIO + 0.5) - L->sub_bottom;

    return top > 0 ? top : 0;
}

static int osd_text_subtitles_present(const VideoState *is) {
    if (!is->subtitle_st || !is->subtitle_st->codecpar) {
        return 0;
    }
    const AVCodecDescriptor *d =
        avcodec_descriptor_get(is->subtitle_st->codecpar->codec_id);
    return d && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

static void osd_info_cache_clear(void) {
    osd_cached_texture_clear(&osd_info_tex);
    if (osd_info_cache.surf) {
        SDL_DestroySurface(osd_info_cache.surf);
        osd_info_cache.surf = NULL;
    }
    osd_info_cache.text[0] = '\0';
    osd_info_cache.box_w = osd_info_cache.box_h = 0;
    osd_info_cache.base_px = osd_info_cache.ss = 0;
    osd_info_cache.pad = 0;
}

static SDL_Surface *osd_info_render_surface(const char *text, int box_w,
                                            int box_h, int line_gap,
                                            int *pad_out) {
    int pad = (int)ceil((double)osd_outline_px * OSD_INFO_SIZE_RATIO) + 2;
    if (pad < 1) {
        pad = 1;
    }
    int sw = box_w + 2 * pad;
    int sh = box_h + 2 * pad;
    SDL_Surface *scratch =
        SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_ARGB8888);
    if (!scratch) {
        return NULL;
    }
    SDL_Renderer *sr = SDL_CreateSoftwareRenderer(scratch);
    if (!sr) {
        SDL_DestroySurface(scratch);
        return NULL;
    }
    SDL_SetRenderDrawBlendMode(sr, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sr, 0, 0, 0, 0);
    SDL_RenderClear(sr);

    SDL_Color fg = {255, 255, 255, 255};
    OsdRect b = {pad, pad, box_w, box_h};
    int used = osd_text_block(sr, &ST_INFO, text, b, line_gap, fg);
    SDL_RenderPresent(sr);
    SDL_DestroyRenderer(sr);

    if (used < 1) {
        used = 1;
    }
    if (used > box_h) {
        used = box_h;
    }

    int out_h = used + 2 * pad;
    SDL_Surface *out = SDL_CreateSurface(sw, out_h, SDL_PIXELFORMAT_ARGB8888);
    if (out) {
        SDL_SetSurfaceBlendMode(scratch, SDL_BLENDMODE_NONE);
        SDL_Rect src = {0, 0, sw, out_h};
        SDL_BlitSurface(scratch, &src, out, NULL);
    }
    SDL_DestroySurface(scratch);
    *pad_out = pad;

    return out;
}

static void osd_draw_info(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    char info[OSD_INFO_TEXT_MAX];
    info[0] = '\0';
    OsdInfoProvider provider =
        osd_info_page == 2 ? osd_stats_provider : osd_info_provider;
    if (provider) {
        provider(is, info, sizeof(info));
    }
    if (!info[0]) {
        return;
    }
    int floor_y;
    if (osd_text_subtitles_present(is)) {
        floor_y = FFMIN(osd_sub_reserved_top(L), L->subs_top) - L->line_gap;
    } else if (L->subs_top < L->ch) {
        floor_y = L->subs_top - L->line_gap;
    } else {
        floor_y = L->ch - L->my;
    }
    int min_floor = L->my + FFMAX(1, (L->ch - 2 * L->my) / 2);
    if (floor_y < min_floor) {
        floor_y = min_floor;
    }

    OsdRect box = {L->mx, L->my, L->cw - L->mx - L->line_gap,
                   FFMAX(1, floor_y - L->my)};
    if (box.w < 1) {
        box.w = 1;
    }

    OsdInfoCache *c = &osd_info_cache;
    if (!c->surf || c->box_w != box.w || c->box_h != box.h ||
        c->base_px != osd_base_px || c->ss != osd_ss ||
        strcmp(c->text, info) != 0) {
        osd_info_cache_clear();
        c->surf =
            osd_info_render_surface(info, box.w, box.h, L->line_gap, &c->pad);
        c->box_w = box.w;
        c->box_h = box.h;
        c->base_px = osd_base_px;
        c->ss = osd_ss;
        snprintf(c->text, sizeof(c->text), "%s", info);
    }

    if (c->surf) {
        osd_draw_cached_surface(r, &osd_info_tex, c->surf,
                                (float)(box.x - c->pad), (float)(box.y - c->pad),
                                SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
}

static void osd_draw_delete(SDL_Renderer *r, OsdLayout *L) {
    SDL_Color fg = {255, 255, 255, 255};
    OsdRect box = {L->mx, L->my, L->topleft.w, L->ch - 2 * L->my};
    osd_text_block(r, &ST_MODAL, osd_delete_prompt, box, L->line_gap, fg);
}

static void osd_draw_volume(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    int vol_pct = (int)(100.0 * is->audio_volume / FFP_MIX_MAXVOLUME + 0.5);
    SDL_Color bar_bg = {80, 80, 80, 200};
    SDL_Color bar_fg = {210, 210, 210, 255};
    SDL_Color bar_hot = {230, 150, 60, 255};
    SDL_Color fg = {255, 255, 255, 255};
    SDL_Color muted_fg = {210, 210, 210, 255};

    int bar_w = L->cw / 4;
    int bar_h = FFMAX(3, osd_px_scale(L->ch, 8.0));
    int bar_pad = FFMAX(1, bar_h / 2);
    int bar_x = (L->cw - bar_w) / 2;
    int seek_stack_top = L->seek_bar_y - L->seek_label_h - L->line_gap;
    int bar_y = seek_stack_top - osd_px_scale(L->ch, 10.0) -
        (bar_h / 2 + bar_pad);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
    SDL_FRect bg_rect = {bar_x - bar_pad, bar_y - bar_h / 2 - bar_pad,
                         bar_w + 2 * bar_pad, bar_h + 2 * bar_pad};
    SDL_RenderFillRect(r, &bg_rect);

    int vol_max = is->audio_volume_max > 0 ? is->audio_volume_max : FFP_MIX_MAXVOLUME;
    int boosted = vol_max > FFP_MIX_MAXVOLUME;
    int unity_px = boosted ? (int)((double)bar_w * FFP_MIX_MAXVOLUME / vol_max + 0.5) : bar_w;
    if (vol_pct > 0 && !is->muted) {
        int filled = (int)((double)bar_w * is->audio_volume / vol_max + 0.5);
        if (filled > bar_w) {
            filled = bar_w;
        }
        int base = filled < unity_px ? filled : unity_px;
        SDL_SetRenderDrawColor(r, bar_fg.r, bar_fg.g, bar_fg.b, bar_fg.a);
        SDL_FRect fg_rect = {bar_x, bar_y - bar_h / 2, base, bar_h};
        SDL_RenderFillRect(r, &fg_rect);
        if (filled > unity_px) {
            SDL_SetRenderDrawColor(r, bar_hot.r, bar_hot.g, bar_hot.b, bar_hot.a);
            SDL_FRect hot_rect = {bar_x + unity_px, bar_y - bar_h / 2, filled - unity_px, bar_h};
            SDL_RenderFillRect(r, &hot_rect);
        }
    }
    if (boosted) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 230);
        SDL_FRect unity_rect = {bar_x + unity_px - 1,
                                bar_y - bar_h / 2 - bar_pad / 2, 2,
                                bar_h + bar_pad};
        SDL_RenderFillRect(r, &unity_rect);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    char vol_label[32];
    snprintf(vol_label, sizeof(vol_label), "%d%%", vol_pct);
    SDL_Color label_col = is->muted ? muted_fg : fg;

    int have_sym = (osd_faces[OSD_FACE_SYM].fill != NULL);
    const char *sym = NULL;
    int sym_w = 0, sym_h = 0;
    if (have_sym) {
        if (is->muted) {
            sym = "\xEE\x84\x8A";
        } else if (vol_pct <= 33) {
            sym = "\xEE\x84\x8B";
        } else if (vol_pct <= 66) {
            sym = "\xEE\x84\x8C";
        } else {
            sym = "\xEE\x84\x8D";
        }
        sym_w = osd_line_width(&ST_SYMBOL, sym);
        sym_h = osd_line_height(&ST_SYMBOL, sym);
    }
    int lw = osd_line_width(&ST_PRIMARY, vol_label);
    int lh = osd_line_height(&ST_PRIMARY, vol_label);
    int gap = sym ? L->row_gap : 0;

    double fit = osd_fit_w(sym_w + gap + lw, L->cw - 2 * L->mx);
    sym_w = (int)(sym_w * fit + 0.5);
    sym_h = (int)(sym_h * fit + 0.5);
    lw = (int)(lw * fit + 0.5);
    lh = (int)(lh * fit + 0.5);
    gap = (int)(gap * fit + 0.5);

    int row_w = sym_w + gap + lw;
    int row_h = sym ? FFMAX(sym_h, lh) : lh;
    int row_y = bar_y - row_h - L->row_gap;
    int row_x = bar_x;
    if (row_x + row_w > L->cw - L->mx) {
        row_x = L->cw - L->mx - row_w;
    }
    if (row_x < L->mx) {
        row_x = L->mx;
    }
    if (sym) {
        osd_line(r, ST_SYMBOL.face, ST_SYMBOL.mono, sym, 0, row_x,
                 row_y + (row_h - sym_h) / 2, label_col,
                 ST_SYMBOL.ratio * fit);
    }
    osd_line(r, ST_PRIMARY.face, ST_PRIMARY.mono, vol_label, 0,
             row_x + sym_w + gap, row_y + (row_h - lh) / 2, label_col,
             ST_PRIMARY.ratio * fit);
}

typedef struct {
    int subtitle, del, info, ab_loop, message, status, volume;
} OsdVis;

/* The actual policy. */
static OsdVis osd_resolve(VideoState *is) {
    int64_t now = (int64_t)SDL_GetTicks();
    OsdVis v = {0};

    v.subtitle = has_active_subtitle(is);
    v.del = osd_delete_prompt_active;
    v.info = osd_info_sticky && !osd_delete_prompt_active && !ab_loop_defining();
    v.ab_loop = ab_loop_defining();
    v.message = (now < osd_message_show_until) && osd_message[0];
    v.status = now < osd_status_show_until;
    v.volume = now < osd_volume_show_until;

    if (v.del) {
        v.info = v.ab_loop = v.message = v.status = v.volume = 0;
    } else if (v.info) {
        v.ab_loop = v.message = v.status = v.volume = 0;
    }

    return v;
}

static void osd_draw_to(SDL_Renderer *r, VideoState *is, int cw, int ch) {
    osd_faces_ensure(ch);
    OsdVis v = osd_resolve(is);
    OsdLayout L;
    osd_layout_init(&L, cw, ch);

    if (v.subtitle) {
        osd_draw_subtitle(r, is, &L);
    }

    if (v.del) {
        osd_draw_delete(r, &L);
        return;
    }
    if (v.info) {
        osd_draw_info(r, is, &L);
        return;
    }

    if (v.ab_loop) {
        osd_draw_abloop(r, &L);
    }
    if (v.message) {
        osd_draw_message(r, &L);
    }
    if (v.status) {
        osd_draw_status(r, is, &L);
    }

    /* Bottom bars. */
    if (v.volume) {
        osd_draw_volume(r, is, &L);
    }
}

static int osd_should_show(VideoState *is) {
    int64_t now = (int64_t)SDL_GetTicks();
    return now < osd_status_show_until ||
        now < osd_volume_show_until || now < osd_message_show_until ||
        osd_info_sticky || osd_delete_prompt_active ||
        has_active_subtitle(is) || ab_loop_defining();
}

static void osd_canvas_size(VideoState *is, int *cw, int *ch) {
    int w = 0, h = 0;
    if (window) {
        SDL_GetWindowSizeInPixels(window, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        w = is->width;
        h = is->height;
    }
    *cw = w;
    *ch = h;
}

/* Vulkan path: render the OSD into a software surface and hand pixels to render_params. */
void osd_prepare_vulkan(VideoState *is) {
    if (!vk_renderer || !osd_faces[OSD_FACE_UI].fill || !osd_should_show(is)) {
        return;
    }

    int cw, ch;
    osd_canvas_size(is, &cw, &ch);
    if (!osd_surface || osd_surface->w != cw || osd_surface->h != ch) {
        if (osd_sw_renderer) {
            osd_invalidate_textures();
            SDL_DestroyRenderer(osd_sw_renderer);
            osd_sw_renderer = NULL;
        }
        SDL_DestroySurface(osd_surface);
        osd_surface = SDL_CreateSurface(cw, ch, SDL_PIXELFORMAT_ABGR8888);
        if (osd_surface) {
            osd_sw_renderer = SDL_CreateSoftwareRenderer(osd_surface);
        }
    }
    if (!osd_sw_renderer) {
        return;
    }

    SDL_SetRenderDrawBlendMode(osd_sw_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(osd_sw_renderer, 0, 0, 0, 0);
    SDL_RenderClear(osd_sw_renderer);

    osd_draw_to(osd_sw_renderer, is, cw, ch);
    /* Don't forget to flush. */
    SDL_RenderPresent(osd_sw_renderer);

    is->render_params.osd_pixels = osd_surface->pixels;
    is->render_params.osd_width = osd_surface->w;
    is->render_params.osd_height = osd_surface->h;
    is->render_params.osd_stride = osd_surface->pitch;
}

/* SDL renderer path: draw the OSD directly into the hardware renderer. */
void osd_draw(VideoState *is) {
    if (!renderer || !osd_faces[OSD_FACE_UI].fill || !osd_should_show(is)) {
        return;
    }

    int cw = 0, ch = 0;
    if (!SDL_GetRenderOutputSize(renderer, &cw, &ch) || cw <= 0 || ch <= 0) {
        osd_canvas_size(is, &cw, &ch);
    }
    osd_draw_to(renderer, is, cw, ch);
}

static void osd_dismiss_info(void) {
    osd_info_sticky = 0;
}

av_printf_format(1, 2) void osd_show_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_message, sizeof(osd_message), fmt, ap);
    va_end(ap);
    osd_message_show_until = (int64_t)SDL_GetTicks() + OSD_MESSAGE_DURATION_MS;
    osd_dismiss_info();
}

void osd_toggle_info_page(int page) {
    if (page != 1 && page != 2) {
        page = 1;
    }
    if (osd_info_sticky && osd_info_page == page) {
        osd_info_sticky = 0;
    } else {
        osd_info_page = page;
        osd_info_sticky = 1;
    }
}

void osd_invalidate_info(void) {
    osd_info_cache_clear();
}

void osd_show_delete_prompt(const char *name) {
    /* Anything besides Y cancels. */
    snprintf(osd_delete_prompt, sizeof(osd_delete_prompt), "Delete %s?",
             name && name[0] ? name : "this file");
    osd_delete_prompt_active = 1;
    osd_info_sticky = 0;
}

void osd_hide_delete_prompt(void) {
    osd_delete_prompt_active = 0;
    osd_delete_prompt[0] = '\0';
}

void osd_set_info_provider(OsdInfoProvider provider) {
    osd_info_provider = provider;
}

void osd_set_stats_provider(OsdInfoProvider provider) {
    osd_stats_provider = provider;
}

void osd_show_position(void) {
    osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_MESSAGE_DURATION_MS;
    osd_dismiss_info();
}

void osd_show_status(void) {
    osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
    osd_dismiss_info();
}

void osd_show_seek(void) {
    osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_SEEK_DURATION_MS;
    osd_dismiss_info();
}

void osd_show_volume(void) {
    osd_volume_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
    osd_dismiss_info();
}

int osd_active(VideoState *is) {
    return osd_should_show(is);
}

int64_t osd_visible_until(void) {
    return FFMAX(FFMAX(osd_status_show_until, osd_message_show_until),
                 osd_volume_show_until);
}

void osd_reset_timers(void) {
    osd_status_show_until = 0;
    osd_volume_show_until = 0;
    osd_message_show_until = 0;
}

static void osd_open_face_set(OsdFaceSet *fs, TTF_Font *(*open)(float), float px) {
    fs->fill = open(px);
    osd_face_setup(fs->fill, 0);
    fs->outline = open(px);
    osd_face_setup(fs->outline, 1);
}

#if !defined(_WIN32) && !defined(__APPLE__)
extern char **environ;

static int osd_fc_match(const char *pattern, char *path, size_t path_size) {
    char *const args[] = {(char *)"fc-match", (char *)"--format=%{file}",
                          (char *)pattern, NULL};
    posix_spawn_file_actions_t fa;
    int fds[2];
    pid_t pid;
    size_t len = 0;
    int status = 0;
    int ok = 0;

    if (path_size < 2 || pipe(fds) != 0) {
        return 0;
    }
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO) ||
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0) ||
        posix_spawn_file_actions_addclose(&fa, fds[0]) ||
        posix_spawnp(&pid, "fc-match", &fa, NULL, args, environ)) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);

    while (len < path_size - 1) {
        ssize_t n = read(fds[0], path + len, path_size - 1 - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fds[0]);

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        ;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        while (len > 0 && (unsigned char)path[len - 1] <= ' ') {
            len--;
        }
        if (len > 0) {
            path[len] = '\0';
            ok = 1;
        }
    }

    return ok;
}
#endif

void osd_init_fonts(void) {
    {
        const char *env = getenv("LACHESIS_OSD_FONT");
        if (env && env[0]) {
            TTF_Font *probe = TTF_OpenFont(env, 18.0f);
            if (probe) {
                TTF_CloseFont(probe);
                snprintf(osd_ui_font_path, sizeof(osd_ui_font_path), "%s", env);
            }
#if !defined(_WIN32) && !defined(__APPLE__)
            else {
                char path[512];
                if (osd_fc_match(env, path, sizeof(path))) {
                    snprintf(osd_ui_font_path, sizeof(osd_ui_font_path), "%s", path);
                }
            }
#endif
        }
    }

    float px = (float)(OSD_FONT_INIT_PX * OSD_SUPERSAMPLE);
    osd_open_face_set(&osd_faces[OSD_FACE_UI], osd_open_ui, px);
    osd_open_face_set(&osd_faces[OSD_FACE_SYM], osd_open_sym, px);
    if (!osd_faces[OSD_FACE_UI].fill) {
        return;
    }

    TTF_Font *ui = osd_faces[OSD_FACE_UI].fill;
    TTF_Font *ui_ol = osd_faces[OSD_FACE_UI].outline;
    char seen_paths[OSD_MAX_FALLBACK_FONTS][512];
    int num_seen = 0;

#if !defined(_WIN32) && !defined(__APPLE__)
    static const char *const fallback_patterns[] = {
        ":lang=ja",
        ":lang=ko",
        ":lang=zh-cn",
        ":lang=zh-tw",
        ":lang=th",
        NULL,
    };
    for (int pi = 0; fallback_patterns[pi] &&
         osd_num_fallback_fonts < OSD_MAX_FALLBACK_FONTS;
         pi++) {
        char path[512];
        if (!osd_fc_match(fallback_patterns[pi], path, sizeof(path))) {
            continue;
        }
        int dup = 0;
        for (int si = 0; si < num_seen; si++) {
            if (!strcmp(seen_paths[si], path)) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        TTF_Font *fb = TTF_OpenFont(path, px);
        if (fb) {
            if (TTF_AddFallbackFont(ui, fb)) {
                if (ui_ol) {
                    TTF_AddFallbackFont(ui_ol, fb);
                }
                snprintf(seen_paths[num_seen++], sizeof(seen_paths[0]), "%s", path);
                osd_fallback_fonts[osd_num_fallback_fonts++] = fb;
            } else {
                TTF_CloseFont(fb);
            }
        }
    }
#else
    static const char *const fallback_paths[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msjh.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",
        "C:\\Windows\\Fonts\\leelawui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Thonburi.ttc",
#endif
        NULL,
    };
    for (int pi = 0; fallback_paths[pi] &&
         osd_num_fallback_fonts < OSD_MAX_FALLBACK_FONTS;
         pi++) {
        const char *path = fallback_paths[pi];
        int dup = 0;
        for (int si = 0; si < num_seen; si++) {
            if (!strcmp(seen_paths[si], path)) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        TTF_Font *fb = TTF_OpenFont(path, px);
        if (fb) {
            if (TTF_AddFallbackFont(ui, fb)) {
                if (ui_ol) {
                    TTF_AddFallbackFont(ui_ol, fb);
                }
                snprintf(seen_paths[num_seen++], sizeof(seen_paths[0]), "%s",
                         path);
                osd_fallback_fonts[osd_num_fallback_fonts++] = fb;
            } else {
                TTF_CloseFont(fb);
            }
        }
    }
#endif
}

void osd_uninit(void) {
    osd_info_cache_clear();
    osd_cached_texture_clear(&osd_sub_tex);
    if (osd_sw_renderer) {
        SDL_DestroyRenderer(osd_sw_renderer);
        osd_sw_renderer = NULL;
    }
    SDL_DestroySurface(osd_surface);
    osd_surface = NULL;

    if (osd_num_fallback_fonts) {
        if (osd_faces[OSD_FACE_UI].fill) {
            TTF_ClearFallbackFonts(osd_faces[OSD_FACE_UI].fill);
        }
        if (osd_faces[OSD_FACE_UI].outline) {
            TTF_ClearFallbackFonts(osd_faces[OSD_FACE_UI].outline);
        }
    }
    for (int i = 0; i < OSD_FACE_COUNT; i++) {
        if (osd_faces[i].fill) {
            TTF_CloseFont(osd_faces[i].fill);
            osd_faces[i].fill = NULL;
        }
        if (osd_faces[i].outline) {
            TTF_CloseFont(osd_faces[i].outline);
            osd_faces[i].outline = NULL;
        }
    }
    for (int fi = 0; fi < osd_num_fallback_fonts; fi++) {
        if (osd_fallback_fonts[fi]) {
            TTF_CloseFont(osd_fallback_fonts[fi]);
            osd_fallback_fonts[fi] = NULL;
        }
    }
    osd_num_fallback_fonts = 0;
    osd_base_px = 0;
    osd_outline_px = 0;
}
