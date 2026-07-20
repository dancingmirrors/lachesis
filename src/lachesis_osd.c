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

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <libavutil/attributes.h>
#include <libavutil/avutil.h>
#include <libavutil/macros.h>

#include "lachesis_osd.h"
#include "lachesis_osd_font.h"
#include "lachesis_osd_ui_font.h"

#define OSD_STATUS_DURATION_MS 1000
#define OSD_SEEK_DURATION_MS 1000
#define OSD_MESSAGE_DURATION_MS 3000
#define OSD_INFO_MAX_LINES 48

#define OSD_FONT_REF_720 55.0
#define OSD_OUTLINE_RATIO (2.0 / 55.0)
#define OSD_INFO_SIZE_RATIO 0.70
#define OSD_SUB_SIZE_RATIO 0.44
#define OSD_INFO_MIN_SCALE 0.70
#define OSD_SUPERSAMPLE 2
#define OSD_FONT_INIT_PX 55
#define OSD_MAX_FALLBACK_FONTS 8
#define OSD_SUB_MAX_TOKENS 512
#define OSD_SUB_MAX_LINES 32
#define OSD_SUB_RESERVE_LINES 2

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
static TTF_Font *osd_emoji_font = NULL;

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
    char text[2048];
    int box_w, box_h;
    int base_px, ss;
    int pad;
} OsdInfoCache;
static OsdInfoCache osd_info_cache;

typedef struct {
    SDL_Surface *surf;
    char text[2048];
    int content_w, content_h;
    int pad;
    int max_w;
    int base_px, ss;
} OsdSubCache;
static OsdSubCache osd_sub_cache;

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
    int cw, ch;
    int mx, my;
    int stack_gap;
    int line_gap;
    int row_gap;
    OsdRect topleft;
    int topleft_cur;
    int sub_bottom;
    int sub_wrap_w;
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

static void osd_ui_measure(TTF_Font *font, const char *text, int *w, int *h) {
    int tw = 0, th = 0;
    if (font && text && text[0]) {
        TTF_GetStringSize(font, text, 0, &tw, &th);
    }
    int ss = osd_ss > 0 ? osd_ss : 1;
    if (w) {
        *w = tw / ss;
    }
    if (h) {
        *h = th / ss;
    }
}

static void osd_draw_tex(SDL_Renderer *r, SDL_Surface *s, float x, float y,
                         double scale, SDL_BlendMode blend) {
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (!t) {
        return;
    }
    SDL_SetTextureBlendMode(t, blend);
    SDL_SetTextureScaleMode(t, scale == 1.0 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_FRect d = {x, y, (float)(s->w * scale), (float)(s->h * scale)};
    SDL_RenderTexture(r, t, NULL, &d);
    SDL_DestroyTexture(t);
}

static SDL_Surface *osd_scale_down(SDL_Surface *raw, int tw, int th) {
    if (!raw || tw < 1 || th < 1) {
        return NULL;
    }
    SDL_Surface *cur = raw;
    int owns_cur = 0;
    while (cur->w >= tw * 2 && cur->h >= th * 2) {
        int hw = cur->w / 2;
        int hh = cur->h / 2;
        SDL_Surface *half = SDL_CreateSurface(hw, hh, SDL_PIXELFORMAT_ARGB8888);
        if (!half) {
            break;
        }
        SDL_SetSurfaceBlendMode(cur, SDL_BLENDMODE_NONE);
        SDL_BlitSurfaceScaled(cur, NULL, half, NULL, SDL_SCALEMODE_LINEAR);
        if (owns_cur) {
            SDL_DestroySurface(cur);
        }
        cur = half;
        owns_cur = 1;
    }
    SDL_Surface *dst = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_ARGB8888);
    if (dst) {
        SDL_SetSurfaceBlendMode(cur, SDL_BLENDMODE_NONE);
        SDL_BlitSurfaceScaled(cur, NULL, dst, NULL, SDL_SCALEMODE_LINEAR);
    }
    if (owns_cur) {
        SDL_DestroySurface(cur);
    }
    return dst;
}

static int osd_utf8_next(const char *text, size_t len, size_t *pos);

static int osd_is_tabular(const char *p, const char *start) {
    if (*p >= '0' && *p <= '9') {
        return 1;
    }
    return (*p == ':' || *p == '.') && p > start && p[-1] >= '0' &&
        p[-1] <= '9' && p[1] >= '0' && p[1] <= '9';
}

static void osd_blit_run(SDL_Renderer *r, OsdFaceSet *fs, const char *text,
                         size_t len, int x, int y, SDL_Color fg, double fscale) {
    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *os =
        fs->outline ? TTF_RenderText_Blended(fs->outline, text, len, black) : NULL;
    SDL_Surface *ft = TTF_RenderText_Blended(fs->fill, text, len, fg);
    if (!ft) {
        SDL_DestroySurface(os);
        return;
    }
    if (os) {
        double off = (double)osd_outline_px * osd_ss * fscale;
        osd_draw_tex(r, os, (float)(x - off), (float)(y - off), fscale,
                     SDL_BLENDMODE_BLEND);
        SDL_DestroySurface(os);
    }
    osd_draw_tex(r, ft, (float)x, (float)y, fscale, SDL_BLENDMODE_BLEND);
    SDL_DestroySurface(ft);
}

static int osd_line(SDL_Renderer *r, OsdFace face, int mono, const char *text,
                    int x, int y, SDL_Color fg, double s) {
    OsdFaceSet *fs = &osd_faces[face];
    if (!fs->fill || !text || !text[0]) {
        return 0;
    }
    int ss = osd_ss > 0 ? osd_ss : 1;
    double fscale = s / ss;

    if (!mono) {
        int w = 0;
        osd_ui_measure(fs->fill, text, &w, NULL);
        if (r) {
            osd_blit_run(r, fs, text, 0, x, y, fg, fscale);
        }
        return (int)(w * s + 0.5);
    }

    int cell_w = 0;
    osd_ui_measure(fs->fill, "0", &cell_w, NULL);
    if (cell_w <= 0) {
        int w = 0;
        osd_ui_measure(fs->fill, text, &w, NULL);
        if (r) {
            osd_blit_run(r, fs, text, 0, x, y, fg, fscale);
        }
        return (int)(w * s + 0.5);
    }

    double cell = cell_w * s;
    double cx = x;
    size_t len = strlen(text), pos = 0;
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
        osd_ui_measure(fs->fill, g, &gw, NULL);
        double adv = gw * s;
        int tab = (clen == 1) && osd_is_tabular(text + st, text);
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

static int osd_text_line(SDL_Renderer *r, const OsdTextStyle *st,
                         const char *text, int x, int y, SDL_Color fg) {
    return osd_line(r, st->face, st->mono, text, x, y, fg, st->ratio);
}

static int osd_line_width(const OsdTextStyle *st, const char *text) {
    SDL_Color fg = {255, 255, 255, 255};
    return osd_line(NULL, st->face, st->mono, text, 0, 0, fg, st->ratio);
}

static int osd_line_height(const OsdTextStyle *st, const char *text) {
    OsdFaceSet *fs = &osd_faces[st->face];
    if (!fs->fill) {
        return 0;
    }
    int h = 0;
    osd_ui_measure(fs->fill, text && text[0] ? text : "0", NULL, &h);
    return (int)(h * st->ratio + 0.5);
}

static double osd_fit_w(int natural_w, int max_w) {
    if (natural_w <= 0 || max_w <= 0 || natural_w <= max_w) {
        return 1.0;
    }
    return (double)max_w / (double)natural_w;
}

static int osd_wrap_into(TTF_Font *font, const char *src, int max_w,
                         char lines[][256], int nline, int cap) {
    if (max_w < 1) {
        max_w = 1;
    }
    int space_w = 0;
    osd_ui_measure(font, " ", &space_w, NULL);

    char cur[256];
    size_t cl = 0;
    int cur_w = 0;
    cur[0] = '\0';

    const char *p = src;
    while (*p && nline < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *ws = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') {
            p++;
        }
        size_t wlen = (size_t)(p - ws);
        char word[256];
        if (wlen >= sizeof(word)) {
            wlen = sizeof(word) - 1;
        }
        memcpy(word, ws, wlen);
        word[wlen] = '\0';
        int ww = 0;
        osd_ui_measure(font, word, &ww, NULL);

        if (cl > 0) {
            if (cur_w + space_w + ww <= max_w && cl + 1 + wlen < sizeof(cur)) {
                cur[cl] = ' ';
                memcpy(cur + cl + 1, word, wlen);
                cl += 1 + wlen;
                cur[cl] = '\0';
                cur_w += space_w + ww;
                continue;
            }
            memcpy(lines[nline++], cur, cl + 1);
            cur[0] = '\0';
            cl = 0;
            cur_w = 0;
            if (nline >= cap) {
                break;
            }
        }

        if (ww <= max_w && wlen < sizeof(cur)) {
            memcpy(cur, word, wlen);
            cl = wlen;
            cur[cl] = '\0';
            cur_w = ww;
            continue;
        }

        size_t wl = strlen(word);
        size_t i = 0;
        char piece[256];
        size_t pl = 0;
        int pw = 0;
        piece[0] = '\0';
        while (i < wl && nline < cap) {
            size_t st = i;
            osd_utf8_next(word, wl, &i);
            size_t clen = i - st;
            char ch[8];
            if (clen >= sizeof(ch)) {
                clen = sizeof(ch) - 1;
            }
            memcpy(ch, word + st, clen);
            ch[clen] = '\0';
            int cw = 0;
            osd_ui_measure(font, ch, &cw, NULL);
            if (pl > 0 && (pw + cw > max_w || pl + clen >= sizeof(piece))) {
                memcpy(lines[nline++], piece, pl + 1);
                pl = 0;
                pw = 0;
                piece[0] = '\0';
                if (nline >= cap) {
                    break;
                }
            }
            memcpy(piece + pl, word + st, clen);
            pl += clen;
            piece[pl] = '\0';
            pw += cw;
        }
        if (pl > 0 && nline < cap) {
            memcpy(cur, piece, pl + 1);
            cl = pl;
            cur_w = pw;
        }
    }
    if (cl > 0 && nline < cap) {
        memcpy(lines[nline++], cur, cl + 1);
    }
    return nline;
}

static int osd_text_block(SDL_Renderer *r, const OsdTextStyle *st,
                          const char *text, OsdRect box, int line_gap,
                          SDL_Color fg, double min_frac) {
    OsdFaceSet *fs = &osd_faces[st->face];
    if (!fs->fill || !text || !text[0] || box.w < 1) {
        return 0;
    }
    int ss = osd_ss > 0 ? osd_ss : 1;
    int fh = TTF_GetFontHeight(fs->fill);
    double nlh = fh > 0 ? (double)fh / ss : 1.0;
    if (nlh < 1.0) {
        nlh = 1.0;
    }
    double ratio = st->ratio;
    int wrap_w = (int)(box.w / ratio + 0.5);
    if (wrap_w < 1) {
        wrap_w = 1;
    }

    char lines[OSD_INFO_MAX_LINES][256];
    int nline = 0;
    const char *p = text;
    while (*p && nline < OSD_INFO_MAX_LINES) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        char seg[256];
        if (seglen >= sizeof(seg)) {
            seglen = sizeof(seg) - 1;
        }
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';
        nline = osd_wrap_into(fs->fill, seg, wrap_w, lines, nline, OSD_INFO_MAX_LINES);
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    if (nline == 0) {
        return 0;
    }

    double total_h = nline * (nlh * ratio) + (double)line_gap * (nline - 1);
    double s = ratio;
    double gap = line_gap;
    if (st->shrink == OSD_SHRINK_FIT && total_h > box.h && total_h > 0) {
        double fit = box.h / total_h;
        s = ratio * fit;
        gap = line_gap * fit;
        if (min_frac > 0.0 && s < ratio * min_frac) {
            s = ratio * min_frac;
            gap = line_gap * min_frac;
        }
    }

    int line_h = (int)(nlh * s + 0.5);
    int step = line_h + (int)(gap + 0.5);
    int ndraw = nline;
    if (step > 0) {
        int fitn = (box.h + (int)(gap + 0.5)) / step;
        if (fitn < 1) {
            fitn = 1;
        }
        if (fitn < ndraw) {
            ndraw = fitn;
        }
    }

    int y = box.y;
    for (int i = 0; i < ndraw; i++) {
        osd_line(r, st->face, st->mono, lines[i], box.x, y, fg, s);
        y += step;
    }
    return (ndraw - 1) * step + line_h;
}

static void subtitle_strip_ass(const char *in, char *out, size_t outsz) {
    if (outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    const char *p = in;
    int skip = 8; /* ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect */
    if (!strncmp(p, "Dialogue:", 9)) {
        p += 9;
        skip = 9; /* Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect */
    }
    for (int i = 0; i < skip && *p; i++) {
        const char *c = strchr(p, ',');
        if (!c) {
            break;
        }
        p = c + 1;
    }

    size_t o = 0;
    while (*p && o + 1 < outsz) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (e) {
                p = e + 1;
                continue;
            }
        }
        if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n')) {
            out[o++] = '\n';
            p += 2;
            continue;
        }
        if (p[0] == '\\' && p[1] == 'h') {
            out[o++] = ' ';
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == '\r' || out[o - 1] == ' ')) {
        out[--o] = '\0';
    }
}

static double osd_display_pos(VideoState *is) {
    double pos = get_master_clock(is);
    if (isnan(pos) && !(is->seek_flags & AVSEEK_FLAG_BYTE)) {
        pos = (double)is->seek_pos / AV_TIME_BASE;
    }
    return pos;
}

static Frame *active_text_subtitle(VideoState *is) {
    if (!is->subtitle_st || frame_queue_nb_remaining(&is->subpq) <= 0) {
        return NULL;
    }
    Frame *sp = frame_queue_peek(&is->subpq);
    if (sp->sub.format == 0) {
        return NULL;
    }
    double clock = get_master_clock(is);
    if (isnan(clock)) {
        return NULL;
    }
    double start = sp->pts + sp->sub.start_display_time / 1000.0;
    double end = sp->pts + sp->sub.end_display_time / 1000.0;
    if (clock < start || clock > end) {
        return NULL;
    }
    return sp;
}

static int has_active_subtitle(VideoState *is) {
    return active_text_subtitle(is) != NULL;
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

static int osd_cp_is_emoji(uint32_t cp) {
    if (!osd_emoji_font || cp < 0x80) {
        return 0;
    }
    if (osd_faces[OSD_FACE_UI].fill &&
        TTF_FontHasGlyph(osd_faces[OSD_FACE_UI].fill, cp)) {
        return 0;
    }
    return TTF_FontHasGlyph(osd_emoji_font, cp);
}

static int osd_cp_is_emoji_join(uint32_t cp) {
    return cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 ||
        (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

static SDL_Surface *osd_render_emoji_scaled(const char *s, size_t len,
                                            int target_h) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *raw = TTF_RenderText_Blended(osd_emoji_font, s, len, white);
    if (!raw) {
        return NULL;
    }
    if (raw->h <= 0 || raw->w <= 0) {
        SDL_DestroySurface(raw);
        return NULL;
    }
    if (raw->h == target_h) {
        return raw;
    }
    int tw = (int)((double)raw->w * target_h / raw->h + 0.5);
    if (tw < 1) {
        tw = 1;
    }
    SDL_Surface *dst = osd_scale_down(raw, tw, target_h);
    SDL_DestroySurface(raw);
    return dst;
}

enum { OSD_SUB_TEXT = 0,
       OSD_SUB_SPACE,
       OSD_SUB_EMOJI,
       OSD_SUB_NEWLINE };

typedef struct {
    int kind;
    size_t start, len;
    SDL_Surface *surf;
    int w, h;
    int relx, line;
} OsdSubToken;

static void osd_sub_cache_clear(void) {
    if (osd_sub_cache.surf) {
        SDL_DestroySurface(osd_sub_cache.surf);
        osd_sub_cache.surf = NULL;
    }
    osd_sub_cache.text[0] = '\0';
    osd_sub_cache.content_w = osd_sub_cache.content_h = 0;
    osd_sub_cache.pad = 0;
    osd_sub_cache.max_w = 0;
    osd_sub_cache.base_px = osd_sub_cache.ss = 0;
}

static SDL_Surface *osd_sub_render_surface(const char *text, size_t used,
                                           int max_w, int *content_w,
                                           int *content_h, int *pad_out) {
    OsdFaceSet *fs = &osd_faces[OSD_FACE_UI];
    int ss = osd_ss > 0 ? osd_ss : 1;
    double ratio = OSD_SUB_SIZE_RATIO;
    double fscale = ratio / ss;
    SDL_Color white = {255, 255, 255, 255};

    int fh = fs->fill ? TTF_GetFontHeight(fs->fill) : 0;
    if (fh <= 0) {
        return NULL;
    }
    int line_h = (int)(fh * fscale + 0.5);
    if (line_h < 1) {
        line_h = 1;
    }
    if (max_w < line_h) {
        max_w = line_h;
    }

    OsdSubToken toks[OSD_SUB_MAX_TOKENS];
    int nalloc = 0;
    size_t pos = 0;
    while (pos < used && nalloc < OSD_SUB_MAX_TOKENS) {
        size_t start = pos;
        uint32_t cp = osd_utf8_next(text, used, &pos);
        if (cp == '\n') {
            toks[nalloc++] = (OsdSubToken){.kind = OSD_SUB_NEWLINE, .line = -1};
            continue;
        }
        if (cp == ' ' || cp == '\t') {
            int w = 0;
            TTF_GetStringSize(fs->fill, " ", 1, &w, NULL);
            toks[nalloc++] = (OsdSubToken){.kind = OSD_SUB_SPACE,
                                           .start = start,
                                           .len = pos - start,
                                           .w = (int)(w * fscale + 0.5),
                                           .h = line_h,
                                           .line = -1};
            continue;
        }
        if (osd_cp_is_emoji(cp)) {
            for (;;) {
                size_t save = pos;
                if (pos >= used) {
                    break;
                }
                uint32_t n = osd_utf8_next(text, used, &pos);
                if (osd_cp_is_emoji(n) || osd_cp_is_emoji_join(n)) {
                    continue;
                }
                pos = save;
                break;
            }
            SDL_Surface *s =
                osd_render_emoji_scaled(text + start, pos - start, line_h);
            if (s) {
                toks[nalloc++] = (OsdSubToken){.kind = OSD_SUB_EMOJI,
                                               .surf = s,
                                               .w = s->w,
                                               .h = s->h,
                                               .line = -1};
            }
            continue;
        }
        for (;;) {
            size_t save = pos;
            if (pos >= used) {
                break;
            }
            uint32_t n = osd_utf8_next(text, used, &pos);
            if (n == '\n' || n == ' ' || n == '\t' || osd_cp_is_emoji(n)) {
                pos = save;
                break;
            }
        }
        int w = 0;
        TTF_GetStringSize(fs->fill, text + start, pos - start, &w, NULL);
        toks[nalloc++] = (OsdSubToken){.kind = OSD_SUB_TEXT,
                                       .start = start,
                                       .len = pos - start,
                                       .w = (int)(w * fscale + 0.5),
                                       .h = line_h,
                                       .line = -1};
    }

    int line_w[OSD_SUB_MAX_LINES] = {0};
    int nlines = 0;
    int cur_w = 0;
    for (int i = 0; i < nalloc && nlines < OSD_SUB_MAX_LINES; i++) {
        if (toks[i].kind == OSD_SUB_NEWLINE) {
            line_w[nlines++] = cur_w;
            cur_w = 0;
            continue;
        }
        if (cur_w > 0 && toks[i].kind != OSD_SUB_SPACE &&
            cur_w + toks[i].w > max_w) {
            line_w[nlines++] = cur_w;
            cur_w = 0;
            if (nlines >= OSD_SUB_MAX_LINES) {
                break;
            }
        }
        toks[i].line = nlines;
        toks[i].relx = cur_w;
        cur_w += toks[i].w;
    }
    if (nlines < OSD_SUB_MAX_LINES) {
        line_w[nlines++] = cur_w;
    }

    int block_w = 0;
    for (int l = 0; l < nlines; l++) {
        if (line_w[l] > block_w) {
            block_w = line_w[l];
        }
    }
    int total_h = nlines * line_h;
    if (block_w < 1 || total_h < 1) {
        for (int i = 0; i < nalloc; i++) {
            if (toks[i].surf) {
                SDL_DestroySurface(toks[i].surf);
            }
        }
        return NULL;
    }

    int pad = (int)ceil((double)osd_outline_px * ss * fscale) + 2;
    if (pad < 1) {
        pad = 1;
    }

    SDL_Surface *surf = SDL_CreateSurface(block_w + 2 * pad, total_h + 2 * pad,
                                          SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *sr = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
    if (!sr) {
        SDL_DestroySurface(surf);
        for (int i = 0; i < nalloc; i++) {
            if (toks[i].surf) {
                SDL_DestroySurface(toks[i].surf);
            }
        }
        return NULL;
    }
    SDL_SetRenderDrawBlendMode(sr, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sr, 0, 0, 0, 0);
    SDL_RenderClear(sr);

    for (int i = 0; i < nalloc; i++) {
        if (toks[i].line < 0) {
            if (toks[i].surf) {
                SDL_DestroySurface(toks[i].surf);
            }
            continue;
        }
        int l = toks[i].line;
        int x = pad + (block_w - line_w[l]) / 2 + toks[i].relx;
        int y = pad + l * line_h;
        if (toks[i].kind == OSD_SUB_EMOJI) {
            if (toks[i].surf) {
                osd_draw_tex(sr, toks[i].surf, (float)x, (float)y, 1.0,
                             SDL_BLENDMODE_BLEND_PREMULTIPLIED);
                SDL_DestroySurface(toks[i].surf);
            }
        } else if (toks[i].kind == OSD_SUB_TEXT) {
            osd_blit_run(sr, fs, text + toks[i].start, toks[i].len, x, y, white,
                         fscale);
        }
    }
    SDL_RenderPresent(sr);
    SDL_DestroyRenderer(sr);

    *content_w = block_w;
    *content_h = total_h;
    *pad_out = pad;
    return surf;
}

static void osd_draw_subtitle(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    Frame *sp = active_text_subtitle(is);
    OsdFaceSet *fs = &osd_faces[OSD_FACE_UI];
    if (!sp || !fs->fill) {
        return;
    }
    int cw = L->cw, ch = L->ch;
    int max_w = L->sub_wrap_w > 0 ? L->sub_wrap_w : (cw > 0 ? cw : (1 << 28));

    char text[2048];
    size_t used = 0;
    text[0] = '\0';
    for (unsigned i = 0; i < sp->sub.num_rects; i++) {
        AVSubtitleRect *rect = sp->sub.rects[i];
        char line[1024];
        if (rect->ass) {
            subtitle_strip_ass(rect->ass, line, sizeof(line));
        } else if (rect->text) {
            snprintf(line, sizeof(line), "%s", rect->text);
        } else {
            line[0] = '\0';
        }
        if (!line[0]) {
            continue;
        }
        if (used && used + 1 < sizeof(text)) {
            text[used++] = '\n';
        }
        int n = snprintf(text + used, sizeof(text) - used, "%s", line);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
        if (used >= sizeof(text)) {
            used = sizeof(text) - 1;
            break;
        }
    }
    text[used] = '\0';
    if (!text[0]) {
        return;
    }

    OsdSubCache *c = &osd_sub_cache;
    if (!c->surf || c->max_w != max_w || c->base_px != osd_base_px ||
        c->ss != osd_ss || strcmp(c->text, text) != 0) {
        osd_sub_cache_clear();
        int w = 0, h = 0, pad = 0;
        c->surf = osd_sub_render_surface(text, used, max_w, &w, &h, &pad);
        c->content_w = w;
        c->content_h = h;
        c->pad = pad;
        c->max_w = max_w;
        c->base_px = osd_base_px;
        c->ss = osd_ss;
        snprintf(c->text, sizeof(c->text), "%s", text);
    }
    if (!c->surf) {
        return;
    }

    int y0 = ch - c->content_h - L->sub_bottom;
    if (y0 < 0) {
        y0 = 0;
    }
    L->subs_top = y0;

    int x = (cw - c->content_w) / 2 - c->pad;
    osd_draw_tex(r, c->surf, (float)x, (float)(y0 - c->pad), 1.0,
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
    L->sub_wrap_w = cw > 0 ? cw * 9 / 10 : 0;
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
    osd_line(r, ST_PRIMARY.face, ST_PRIMARY.mono, line, b.x, b.y, fg,
             ST_PRIMARY.ratio * fit);
    osd_stack_advance(L, th);
}

static void osd_draw_message(SDL_Renderer *r, OsdLayout *L) {
    SDL_Color fg = {255, 255, 255, 255};
    OsdRect b = osd_stack_remaining(L, L->ch - L->my);
    int consumed =
        osd_text_block(r, &ST_MESSAGE, osd_message, b, L->line_gap, fg, 0.0);
    osd_stack_advance(L, consumed);
}

static void osd_draw_status(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    double pos = osd_display_pos(is);
    double dur = (is->ic && is->ic->duration != AV_NOPTS_VALUE)
        ? (double)is->ic->duration / AV_TIME_BASE
        : 0.0;
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
        osd_line(r, ST_SYMBOL.face, ST_SYMBOL.mono, sym, b.x,
                 b.y + (row_h - sh) / 2, fg, ST_SYMBOL.ratio * fit);
    }
    osd_line(r, ST_PRIMARY.face, ST_PRIMARY.mono, line, b.x + sw + gap,
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
    OsdFaceSet *fs = &osd_faces[OSD_FACE_UI];
    int ss = osd_ss > 0 ? osd_ss : 1;
    int fh = fs->fill ? TTF_GetFontHeight(fs->fill) : 0;
    int line_h = (int)(fh * OSD_SUB_SIZE_RATIO / ss + 0.5);
    if (line_h < 1) {
        line_h = 1;
    }
    int top = L->ch - (OSD_SUB_RESERVE_LINES * line_h + L->sub_bottom);
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
    int used = osd_text_block(sr, &ST_INFO, text, b, line_gap, fg,
                              OSD_INFO_MIN_SCALE);
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
    char info[2048];
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
        osd_draw_tex(r, c->surf, (float)(box.x - c->pad),
                     (float)(box.y - c->pad), 1.0,
                     SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
}

static void osd_draw_delete(SDL_Renderer *r, OsdLayout *L) {
    SDL_Color fg = {255, 255, 255, 255};
    OsdRect box = {L->mx, L->my, L->topleft.w, L->ch - 2 * L->my};
    osd_text_block(r, &ST_MODAL, osd_delete_prompt, box, L->line_gap, fg, 0.0);
}

static void osd_draw_volume(SDL_Renderer *r, VideoState *is, OsdLayout *L) {
    int vol_pct = (int)(100.0 * is->audio_volume / FFP_MIX_MAXVOLUME + 0.5);
    SDL_Color bar_bg = {80, 80, 80, 200};
    SDL_Color bar_fg = {210, 210, 210, 255};
    SDL_Color bar_hot = {230, 150, 60, 255};
    SDL_Color fg = {255, 255, 255, 255};
    SDL_Color muted_fg = {210, 210, 210, 255};

    int bar_w = L->cw / 4;
    int bar_h = 8;
    int bar_x = (L->cw - bar_w) / 2;
    int seek_stack_top = L->seek_bar_y - L->seek_label_h - L->line_gap;
    int bar_y = seek_stack_top - osd_px_scale(L->ch, 10.0) - (bar_h / 2 + 4);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
    SDL_FRect bg_rect = {bar_x - 4, bar_y - bar_h / 2 - 4, bar_w + 8, bar_h + 8};
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
        SDL_FRect unity_rect = {bar_x + unity_px - 1, bar_y - bar_h / 2 - 2, 2, bar_h + 4};
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
    (void)lw;
    int gap = sym ? L->row_gap : 0;
    int row_h = sym ? FFMAX(sym_h, lh) : lh;
    int row_y = bar_y - row_h - L->row_gap;
    if (sym) {
        osd_text_line(r, &ST_SYMBOL, sym, bar_x, row_y + (row_h - sym_h) / 2,
                      label_col);
    }
    osd_text_line(r, &ST_PRIMARY, vol_label, bar_x + sym_w + gap,
                  row_y + (row_h - lh) / 2, label_col);
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
                int safe = 1;
                for (const char *p = env; *p; p++) {
                    char c = *p;
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                          c == '_' || c == ':' || c == '.' || c == ',')) {
                        safe = 0;
                        break;
                    }
                }
                if (safe) {
                    char cmd[600];
                    snprintf(cmd, sizeof(cmd),
                             "fc-match --format=%%{file} \"%s\" 2>/dev/null", env);
                    FILE *fp = popen(cmd, "r");
                    if (fp) {
                        char path[512] = {0};
                        if (fgets(path, sizeof(path), fp) && path[0]) {
                            snprintf(osd_ui_font_path, sizeof(osd_ui_font_path),
                                     "%s", path);
                        }
                        pclose(fp);
                    }
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
    {
        FILE *fp = popen("fc-match --format=%{file} emoji 2>/dev/null", "r");
        if (fp) {
            char path[512] = {0};
            if (fgets(path, sizeof(path), fp) && path[0]) {
                osd_emoji_font = TTF_OpenFont(path, 64.0f);
                if (osd_emoji_font) {
                    snprintf(seen_paths[num_seen++], sizeof(seen_paths[0]), "%s",
                             path);
                }
            }
            pclose(fp);
        }
    }
#else
    {
        static const char *const emoji_paths[] = {
#if defined(_WIN32)
            "C:\\Windows\\Fonts\\seguiemj.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/Apple Color Emoji.ttc",
#endif
            NULL,
        };
        for (int ei = 0; emoji_paths[ei] && !osd_emoji_font; ei++) {
            osd_emoji_font = TTF_OpenFont(emoji_paths[ei], 64.0f);
            if (osd_emoji_font) {
                snprintf(seen_paths[num_seen++], sizeof(seen_paths[0]), "%s",
                         emoji_paths[ei]);
            }
        }
    }
#endif

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
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "fc-match --format=%%{file} \"%s\" 2>/dev/null",
                 fallback_patterns[pi]);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            continue;
        }
        char path[512] = {0};
        if (fgets(path, sizeof(path), fp) && path[0]) {
            int dup = 0;
            for (int si = 0; si < num_seen; si++) {
                if (!strcmp(seen_paths[si], path)) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                TTF_Font *fb = TTF_OpenFont(path, px);
                if (fb) {
                    if (TTF_AddFallbackFont(ui, fb)) {
                        if (ui_ol) {
                            TTF_AddFallbackFont(ui_ol, fb);
                        }
                        snprintf(seen_paths[num_seen++], sizeof(seen_paths[0]),
                                 "%s", path);
                        osd_fallback_fonts[osd_num_fallback_fonts++] = fb;
                    } else {
                        TTF_CloseFont(fb);
                    }
                }
            }
        }
        pclose(fp);
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
    osd_sub_cache_clear();
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
    if (osd_emoji_font) {
        TTF_CloseFont(osd_emoji_font);
        osd_emoji_font = NULL;
    }
    osd_base_px = 0;
    osd_outline_px = 0;
}
