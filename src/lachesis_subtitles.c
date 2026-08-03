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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <ass/ass.h>
#include <zlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/macros.h>

#include <SDL3/SDL.h>

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_osd_emoji_font.h"
#include "lachesis_subtitles.h"

#define ASS_EMOJI_FAMILY "Noto Emoji"
#define ASS_EVENT_MAX 8192

static ASS_Library *ass_library;
static ASS_Renderer *ass_renderer;
static ASS_Track *ass_track;
static SDL_Mutex *ass_lock;

static int ass_frame_w, ass_frame_h;
static int ass_storage_w, ass_storage_h;

static int ass_text_readorder;

static SDL_Surface *ass_surface;
static int ass_surface_x, ass_surface_y;
static int ass_surface_stale;
static unsigned ass_generation;

static void ass_message_cb(int level, const char *fmt, va_list va, void *data) {
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

static void ass_engine_uninit_locked(void) {
    if (ass_track) {
        ass_free_track(ass_track);
        ass_track = NULL;
    }
    if (ass_renderer) {
        ass_renderer_done(ass_renderer);
        ass_renderer = NULL;
    }
    if (ass_library) {
        ass_library_done(ass_library);
        ass_library = NULL;
    }
    if (ass_surface) {
        SDL_DestroySurface(ass_surface);
        ass_surface = NULL;
    }
    ass_frame_w = ass_frame_h = 0;
    ass_storage_w = ass_storage_h = 0;
}

static void ass_add_emoji_font_locked(void);

static int ass_engine_init_locked(void) {
    size_t font_size = 0;
    const void *font_data;

    if (ass_renderer) {
        return 0;
    }

    if (!ass_library) {
        ass_library = ass_library_init();
        if (!ass_library) {
            log_warn("Could not initialize libass.\n");
            return -1;
        }
        ass_set_message_cb(ass_library, ass_message_cb, NULL);
        ass_set_extract_fonts(ass_library, 1);

        /* The last resort. */
        font_data = osd_embedded_ui_font(&font_size);
        if (font_data && font_size) {
            ass_add_font(ass_library, "lachesis-ui", font_data, font_size);
        }

        ass_add_emoji_font_locked();
    }

    ass_renderer = ass_renderer_init(ass_library);
    if (!ass_renderer) {
        log_warn("Could not initialize the libass renderer.\n");
        return -1;
    }

    ass_set_shaper(ass_renderer, ASS_SHAPING_COMPLEX);
    ass_set_hinting(ass_renderer, ASS_HINTING_NONE);
    ass_set_fonts(ass_renderer, NULL, "Arial", ASS_FONTPROVIDER_AUTODETECT,
                  NULL, 1);

    return 0;
}

void subtitles_init(void) {
    if (!ass_lock) {
        ass_lock = SDL_CreateMutex();
    }
}

static int ass_have_lock(void) {
    return ass_lock != NULL;
}

static void ass_add_emoji_font_locked(void) {
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

    ass_add_font(ass_library, "NotoEmoji", (const char *)buf, (int)len);
    av_free(buf);
}

static uint32_t ass_utf8_next(const char *s, size_t len, size_t *pos) {
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

static int cp_is_emoji_base(uint32_t cp) {
    return cp >= 0x1F000 && cp <= 0x1FAFF;
}

static int cp_is_emoji_join(uint32_t cp) {
    return cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 ||
        (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

static const char *ass_style_font_locked(const char *name, size_t len) {
    if (!ass_track) {
        return NULL;
    }
    for (int i = 0; i < ass_track->n_styles; i++) {
        const ASS_Style *st = &ass_track->styles[i];

        if (st->Name && strlen(st->Name) == len && !strncmp(st->Name, name, len)) {
            return st->FontName;
        }
    }
    if (ass_track->default_style >= 0 &&
        ass_track->default_style < ass_track->n_styles) {
        return ass_track->styles[ass_track->default_style].FontName;
    }

    return NULL;
}

static int ass_route_emoji_locked(const char *in, char *out, size_t outsz) {
    const char *style = NULL, *text = NULL;
    const char *cur_font;
    char held[256];
    size_t style_len = 0, o = 0, p, tlen;
    int fields = 0;
    int in_override = 0;
    int found = 0;

    /* ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text */
    for (const char *c = in; *c; c++) {
        if (*c != ',') {
            continue;
        }
        fields++;
        if (fields == 2) {
            style = c + 1;
        } else if (fields == 3) {
            style_len = (size_t)(c - style);
        } else if (fields == 8) {
            text = c + 1;
            break;
        }
    }
    if (!text || !style) {
        return 0;
    }

    cur_font = ass_style_font_locked(style, style_len);
    if (!cur_font || !*cur_font) {
        return 0;
    }

    tlen = strlen(text);
    for (p = 0; p < tlen;) {
        size_t save = p;
        uint32_t cp = ass_utf8_next(text, tlen, &p);

        if (cp == '{') {
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

    o = (size_t)(text - in);
    if (o >= outsz) {
        return 0;
    }
    memcpy(out, in, o);

#define EMIT(str, n) \
    do { \
        if (o + (n) >= outsz) { \
            return 0; \
        } \
        memcpy(out + o, (str), (n)); \
        o += (n); \
    } while (0)

    in_override = 0;
    for (p = 0; p < tlen;) {
        size_t start = p;
        uint32_t cp = ass_utf8_next(text, tlen, &p);

        if (p == start) {
            break;
        }

        if (in_override) {
            if (cp == '\\' && p < tlen) {
                if (!strncmp(text + p, "fn", 2)) {
                    const char *fn = text + p + 2;
                    size_t n = strcspn(fn, "\\}");

                    if (n && n < sizeof(held)) {
                        memcpy(held, fn, n);
                        held[n] = '\0';
                        cur_font = held;
                    }
                } else if (text[p] == 'r') {
                    const char *rn = text + p + 1;
                    size_t n = strcspn(rn, "\\}");
                    const char *f = n ? ass_style_font_locked(rn, n)
                                      : ass_style_font_locked(style, style_len);

                    if (f && *f) {
                        cur_font = f;
                    }
                }
            }
            if (cp == '}') {
                in_override = 0;
            }
            EMIT(text + start, p - start);
            continue;
        }

        if (cp == '{') {
            in_override = 1;
            EMIT(text + start, p - start);
            continue;
        }

        if (cp_is_emoji_base(cp)) {
            EMIT("{\\fn" ASS_EMOJI_FAMILY "}",
                 sizeof("{\\fn" ASS_EMOJI_FAMILY "}") - 1);
            for (;;) {
                size_t save = p;
                uint32_t n;

                if (p >= tlen) {
                    break;
                }
                n = ass_utf8_next(text, tlen, &p);
                if (cp_is_emoji_base(n) || cp_is_emoji_join(n)) {
                    continue;
                }
                p = save;
                break;
            }
            EMIT(text + start, p - start);
            EMIT("{\\fn", 4);
            EMIT(cur_font, strlen(cur_font));
            EMIT("}", 1);
            continue;
        }

        if (p < tlen) {
            size_t peek = p;

            if (ass_utf8_next(text, tlen, &peek) == 0xFE0F) {
                EMIT("{\\fn" ASS_EMOJI_FAMILY "}",
                     sizeof("{\\fn" ASS_EMOJI_FAMILY "}") - 1);
                EMIT(text + start, peek - start);
                EMIT("{\\fn", 4);
                EMIT(cur_font, strlen(cur_font));
                EMIT("}", 1);
                p = peek;
                continue;
            }
        }

        EMIT(text + start, p - start);
    }

    if (o >= outsz) {
        return 0;
    }
    out[o] = '\0';

#undef EMIT

    return 1;
}

static int subtitle_is_text(const AVSubtitle *sub) {
    for (unsigned i = 0; i < sub->num_rects; i++) {
        enum AVSubtitleType type = sub->rects[i]->type;

        if (type == SUBTITLE_ASS || type == SUBTITLE_TEXT) {
            return 1;
        }
    }

    return 0;
}

static int subtitles_feed(const AVSubtitle *sub, double pts) {
    long long start_ms, duration_ms;
    int consumed = 0;

    if (!sub->num_rects) {
        return 0;
    }

    if (!ass_have_lock()) {
        return 0;
    }

    SDL_LockMutex(ass_lock);
    if (!ass_track) {
        SDL_UnlockMutex(ass_lock);
        return 0;
    }

    start_ms = (long long)(pts * 1000.0) + sub->start_display_time;
    duration_ms = (long long)sub->end_display_time - (long long)sub->start_display_time;
    if (duration_ms < 0) {
        duration_ms = 0;
    }

    for (unsigned i = 0; i < sub->num_rects; i++) {
        const AVSubtitleRect *rect = sub->rects[i];

        if (rect->type == SUBTITLE_ASS && rect->ass) {
            char routed[ASS_EVENT_MAX];
            const char *line = rect->ass;

            if (ass_route_emoji_locked(rect->ass, routed, sizeof(routed))) {
                line = routed;
            }
            ass_process_chunk(ass_track, (char *)line, (int)strlen(line),
                              start_ms, duration_ms);
            consumed = 1;
        } else if (rect->type == SUBTITLE_TEXT && rect->text) {
            char line[4096];
            int n = snprintf(line, sizeof(line), "%d,0,Default,,0,0,0,,%s",
                             ass_text_readorder++, rect->text);

            if (n > 0) {
                char *p = line;

                if ((size_t)n >= sizeof(line)) {
                    n = (int)sizeof(line) - 1;
                }
                for (; *p; p++) {
                    if (*p == '\n' || *p == '\r') {
                        *p = ' ';
                    }
                }
                ass_process_chunk(ass_track, line, n, start_ms, duration_ms);
                consumed = 1;
            }
        }
    }
    SDL_UnlockMutex(ass_lock);

    return consumed;
}

int subtitles_track_open(AVCodecContext *avctx) {
    const AVCodecDescriptor *desc;
    int is_text;

    if (!avctx) {
        return -1;
    }

    if (!ass_have_lock()) {
        return -1;
    }

    desc = avcodec_descriptor_get(avctx->codec_id);
    is_text = desc && (desc->props & AV_CODEC_PROP_TEXT_SUB);

    subtitles_track_close();

    if (!is_text) {
        return 0;
    }

    SDL_LockMutex(ass_lock);
    if (ass_engine_init_locked() < 0) {
        SDL_UnlockMutex(ass_lock);
        return -1;
    }

    ass_track = ass_new_track(ass_library);
    if (!ass_track) {
        SDL_UnlockMutex(ass_lock);
        log_warn("Could not allocate a libass track.\n");
        return -1;
    }

    if (avctx->subtitle_header && avctx->subtitle_header_size > 0) {
        ass_process_codec_private(ass_track, (char *)avctx->subtitle_header,
                                  avctx->subtitle_header_size);
    }

    ass_surface_stale = 1;
    ass_generation++;

    SDL_UnlockMutex(ass_lock);

    return 0;
}

void subtitles_track_close(void) {
    if (!ass_lock) {
        return;
    }
    SDL_LockMutex(ass_lock);
    if (ass_track) {
        ass_free_track(ass_track);
        ass_track = NULL;
    }
    ass_surface_stale = 1;
    ass_generation++;
    SDL_UnlockMutex(ass_lock);
}

void subtitles_track_flush(void) {
    if (!ass_lock) {
        return;
    }
    SDL_LockMutex(ass_lock);
    if (ass_track) {
        ass_flush_events(ass_track);
    }
    ass_text_readorder = 0;
    SDL_UnlockMutex(ass_lock);
}

void subtitles_reap(void) {
    if (!ass_lock) {
        return;
    }
    SDL_LockMutex(ass_lock);
    if (ass_surface_stale) {
        ass_surface_stale = 0;
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
        }
    }
    SDL_UnlockMutex(ass_lock);
}

void subtitles_uninit(void) {
    if (ass_lock) {
        SDL_LockMutex(ass_lock);
        ass_engine_uninit_locked();
        SDL_UnlockMutex(ass_lock);
        SDL_DestroyMutex(ass_lock);
        ass_lock = NULL;
    } else {
        ass_engine_uninit_locked();
    }
}

int subtitles_track_attached(void) {
    int attached;

    if (!ass_lock) {
        return 0;
    }
    SDL_LockMutex(ass_lock);
    attached = ass_track != NULL;
    SDL_UnlockMutex(ass_lock);

    return attached;
}

int subtitles_visible_at(double now) {
    long long now_ms;
    int visible = 0;

    if (!ass_lock || isnan(now)) {
        return 0;
    }

    now_ms = (long long)(now * 1000.0);

    SDL_LockMutex(ass_lock);
    if (ass_track) {
        for (int i = 0; i < ass_track->n_events; i++) {
            const ASS_Event *e = &ass_track->events[i];

            if (now_ms >= e->Start && now_ms < e->Start + e->Duration) {
                visible = 1;
                break;
            }
        }
    }
    SDL_UnlockMutex(ass_lock);

    return visible;
}

static void ass_blend_image(uint32_t *pixels, int pitch_px, int surf_w,
                            int surf_h, const ASS_Image *img, int ox, int oy) {
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
            da = (d >> 24) & 0xFF;
            dr = (d >> 16) & 0xFF;
            dg = (d >> 8) & 0xFF;
            db = d & 0xFF;

            dr = (sr * a + 127) / 255 + (dr * inv + 127) / 255;
            dg = (sg * a + 127) / 255 + (dg * inv + 127) / 255;
            db = (sb * a + 127) / 255 + (db * inv + 127) / 255;
            da = a + (da * inv + 127) / 255;

            dst[dx] = (FFMIN(da, 255u) << 24) | (FFMIN(dr, 255u) << 16) |
                (FFMIN(dg, 255u) << 8) | FFMIN(db, 255u);
        }
    }
}

static int ass_composite_locked(ASS_Image *img, int frame_w, int frame_h) {
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    uint32_t *pixels;
    int pitch_px;

    for (ASS_Image *p = img; p; p = p->next) {
        if (p->w <= 0 || p->h <= 0) {
            continue;
        }
        min_x = FFMIN(min_x, p->dst_x);
        min_y = FFMIN(min_y, p->dst_y);
        max_x = FFMAX(max_x, p->dst_x + p->w);
        max_y = FFMAX(max_y, p->dst_y + p->h);
    }

    if (min_x > max_x || min_y > max_y) {
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
        }
        return 0;
    }

    min_x = FFMAX(min_x, 0);
    min_y = FFMAX(min_y, 0);
    max_x = FFMIN(max_x, frame_w);
    max_y = FFMIN(max_y, frame_h);
    if (max_x <= min_x || max_y <= min_y) {
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
        }
        return 0;
    }

    if (ass_surface) {
        SDL_DestroySurface(ass_surface);
    }
    ass_surface = SDL_CreateSurface(max_x - min_x, max_y - min_y,
                                    SDL_PIXELFORMAT_ARGB8888);
    if (!ass_surface) {
        return 0;
    }
    SDL_ClearSurface(ass_surface, 0.0f, 0.0f, 0.0f, 0.0f);

    pixels = ass_surface->pixels;
    pitch_px = ass_surface->pitch / 4;
    for (ASS_Image *p = img; p; p = p->next) {
        if (p->w <= 0 || p->h <= 0) {
            continue;
        }
        ass_blend_image(pixels, pitch_px, ass_surface->w, ass_surface->h, p,
                        p->dst_x - min_x, p->dst_y - min_y);
    }

    ass_surface_x = min_x;
    ass_surface_y = min_y;

    return 1;
}

int subtitles_render(VideoState *is, int canvas_w, int canvas_h,
                     const SDL_Rect *video_rect, double now,
                     SubtitleOverlay *out) {
    int frame_w, frame_h, origin_x, origin_y;
    int storage_w = 0, storage_h = 0;
    int changed = 0, geometry_changed = 0;
    ASS_Image *img;
    long long now_ms;

    if (!ass_lock || isnan(now) || canvas_w <= 0 || canvas_h <= 0) {
        return 0;
    }

    if (video_rect && video_rect->w > 0 && video_rect->h > 0) {
        frame_w = video_rect->w;
        frame_h = video_rect->h;
        origin_x = video_rect->x;
        origin_y = video_rect->y;
    } else {
        frame_w = canvas_w;
        frame_h = canvas_h;
        origin_x = origin_y = 0;
    }

    if (is && is->video_st && is->video_st->codecpar) {
        storage_w = is->video_st->codecpar->width;
        storage_h = is->video_st->codecpar->height;
    }

    now_ms = (long long)(now * 1000.0);

    SDL_LockMutex(ass_lock);

    if (ass_surface_stale) {
        ass_surface_stale = 0;
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
        }
    }

    if (!ass_track || !ass_renderer) {
        SDL_UnlockMutex(ass_lock);
        return 0;
    }

    if (ass_frame_w != frame_w || ass_frame_h != frame_h) {
        ass_frame_w = frame_w;
        ass_frame_h = frame_h;
        ass_set_frame_size(ass_renderer, frame_w, frame_h);
        geometry_changed = 1;
    }
    if (ass_storage_w != storage_w || ass_storage_h != storage_h) {
        ass_storage_w = storage_w;
        ass_storage_h = storage_h;
        ass_set_storage_size(ass_renderer, storage_w, storage_h);
        geometry_changed = 1;
    }

    img = ass_render_frame(ass_renderer, ass_track, now_ms, &changed);
    if (!img) {
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
            ass_generation++;
        }
        SDL_UnlockMutex(ass_lock);
        return 0;
    }

    if (changed || geometry_changed || !ass_surface) {
        ass_generation++;
        if (!ass_composite_locked(img, frame_w, frame_h)) {
            SDL_UnlockMutex(ass_lock);
            return 0;
        }
    }

    out->surf = ass_surface;
    out->x = origin_x + ass_surface_x;
    out->y = origin_y + ass_surface_y;
    out->generation = ass_generation;
    SDL_UnlockMutex(ass_lock);

    return 1;
}

int subtitle_thread(void *arg) {
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq))) {
            return 0;
        }

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0) {
            break;
        }

        pts = 0;

        if (got_subtitle) {
            if (sp->sub.pts != AV_NOPTS_VALUE) {
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            }

            if (subtitle_is_text(&sp->sub)) {
                if (is->subdec.pkt_serial == is->subtitleq.serial &&
                    !subtitles_feed(&sp->sub, pts)) {
                    static int feed_warned = 0;

                    if (!feed_warned) {
                        feed_warned = 1;
                        log_warn("No libass track for this subtitle stream. "
                                 "Text subtitles will not be shown.\n");
                    }
                }
                avsubtitle_free(&sp->sub);
                continue;
            }

            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* Now we can update the picture count. */
            frame_queue_push(&is->subpq);
        }
    }

    return 0;
}

static int sub_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request || is->sub_seek_pending;
}

static int sub_read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = is->sub_ic;
    AVPacket *pkt = av_packet_alloc();
    int sent_eof = 0;

    if (!pkt) {
        return AVERROR(ENOMEM);
    }

    for (;;) {
        if (is->abort_request || is->sub_abort_request) {
            break;
        }

        if (is->sub_seek_pending) {
            avformat_seek_file(ic, -1,
                               is->sub_seek_min,
                               is->sub_seek_pos,
                               is->sub_seek_max,
                               is->sub_seek_flags);
            is->sub_seek_pending = 0;
            sent_eof = 0;
            subtitles_track_flush();
            continue;
        }

        if (!sent_eof &&
            stream_has_enough_packets(is->subtitle_st, is->sub_ext_stream, &is->subtitleq)) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                sent_eof = 1;
                SDL_Delay(50);
            } else if (!is->sub_seek_pending && !is->abort_request) {
                SDL_Delay(10);
            }
            continue;
        }

        if (sent_eof) {
            sent_eof = 0;
        }

        if (pkt->stream_index == is->sub_ext_stream) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);

    return 0;
}

int open_external_subtitle(VideoState *is) {
    static const char *const sub_exts[] = {
        "srt", "ass", "ssa", "vtt", "sub", NULL};

    if (!is->filename || is->subtitle_st || subtitle_disable) {
        return -1;
    }
    /* XXX */
    if (is->archive_path || strstr(is->filename, "://")) {
        return -1;
    }

    const char *dot = strrchr(is->filename, '.');
    const char *slash = strrchr(is->filename, '/');
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    size_t base_len = dot ? (size_t)(dot - is->filename) : strlen(is->filename);

    char path[4096];
    int found = 0;
    for (int i = 0; sub_exts[i]; i++) {
        snprintf(path, sizeof(path), "%.*s.%s", (int)base_len, is->filename, sub_exts[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }

    AVFormatContext *sic = avformat_alloc_context();
    if (!sic) {
        return AVERROR(ENOMEM);
    }
    sic->interrupt_callback.callback = sub_interrupt_cb;
    sic->interrupt_callback.opaque = is;

    int idx;
    AVStream *st;
    const AVCodec *codec;
    AVCodecContext *avctx;

    int ret = avformat_open_input(&sic, path, NULL, NULL);
    if (ret < 0) {
        log_warn("Could not open external subtitle '%s'!\n", path);
        return ret;
    }
    if ((ret = avformat_find_stream_info(sic, NULL)) < 0) {
        goto fail;
    }

    idx = av_find_best_stream(sic, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (idx < 0) {
        ret = idx;
        goto fail;
    }

    st = sic->streams[idx];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (subtitle_codec_name) {
        codec = avcodec_find_decoder_by_name(subtitle_codec_name);
    }
    if (!codec) {
        ret = AVERROR_DECODER_NOT_FOUND;
        goto fail;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = avcodec_parameters_to_context(avctx, st->codecpar);
    if (ret < 0) {
        avcodec_free_context(&avctx);
        goto fail;
    }
    avctx->pkt_timebase = st->time_base;
    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        avcodec_free_context(&avctx);
        goto fail;
    }

    subtitles_track_open(avctx);

    if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0) {
        subtitles_track_close();
        avcodec_free_context(&avctx);
        goto fail;
    }

    is->sub_ic = sic;
    is->sub_ext_stream = idx;
    is->subtitle_st = st;

    if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0) {
        decoder_destroy(&is->subdec);
        subtitles_track_close();
        is->subtitle_st = NULL;
        is->sub_ic = NULL;
        goto fail;
    }

    is->sub_read_tid = SDL_CreateThread(sub_read_thread, "sub_reader", is);
    if (!is->sub_read_tid) {
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        subtitles_track_close();
        is->subtitle_st = NULL;
        is->sub_ic = NULL;
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    log_info("Loaded external subtitle: %s\n", path);
    return 0;

fail:
    avformat_close_input(&sic);
    return ret;
}

void close_external_subtitle(VideoState *is) {
    if (!is->sub_ic) {
        return;
    }

    is->sub_abort_request = 1;
    if (is->sub_read_tid) {
        SDL_WaitThread(is->sub_read_tid, NULL);
        is->sub_read_tid = NULL;
    }

    decoder_abort(&is->subdec, &is->subpq);
    decoder_destroy(&is->subdec);
    subtitles_track_close();

    is->subtitle_st = NULL;
    is->sub_ext_stream = -1;
    avformat_close_input(&is->sub_ic);
    is->sub_abort_request = 0;
}
