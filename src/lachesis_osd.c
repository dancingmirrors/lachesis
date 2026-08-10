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

#include <ass/ass.h>

#include <SDL3/SDL.h>

#include <libavutil/attributes.h>
#include <libavutil/avutil.h>
#include <libavutil/macros.h>

#include "lachesis_ass.h"
#include "lachesis_log.h"
#include "lachesis_osd.h"
#include "lachesis_subtitles.h"

#define OSD_STATUS_DURATION_MS 1000
#define OSD_SEEK_DURATION_MS 1000
#define OSD_MESSAGE_DURATION_MS 3000
#define OSD_INFO_TEXT_MAX 8192

#define OSD_RES_H 720.0
#define OSD_FONT_SIZE 66.0
#define OSD_OUTLINE 2.0
#define OSD_MIN_FONT 2.0
#define OSD_INFO_SIZE_RATIO 0.70
#define OSD_SUB_RESERVE_RATIO 0.09

#define OSD_MEASURE_RES 16384
#define OSD_MEASURE_SCALE 2
#define OSD_MEASURE_ORIGIN 512
#define OSD_FIT_PASSES 5

#define OSD_EVENTS_MAX 12
#define OSD_BODY_MAX (2 * OSD_INFO_TEXT_MAX)
#define OSD_EVENT_BUF (OSD_BODY_MAX + 4096)

#define OSD_WRAP_LINE 1
#define OSD_WRAP_NONE 2

typedef struct {
    double x, y, w, h;
} OsdRect;

typedef struct {
    double w;
    double h;
    int lines;
} OsdMetrics;

typedef struct {
    double w;
    double mx, my;
    double stack_gap, line_gap, row_gap;
    double topleft_w;
    double topleft_cur;
    double sub_bottom;
    double subs_top;
    double seek_bar_y;
    double seek_label_h;
} OsdLayout;

static ASS_Library *osd_library;
static ASS_Renderer *osd_renderer;
static ASS_Track *osd_track;
static ASS_Renderer *osd_measure_renderer;
static ASS_Track *osd_measure_track;
static int osd_engine_failed;

static char osd_font_family[256];

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

static char osd_ev_buf[OSD_EVENT_BUF];
static size_t osd_ev_len;
static size_t osd_ev_off[OSD_EVENTS_MAX];
static int osd_ev_margin_r[OSD_EVENTS_MAX];
static int osd_ev_count;

static char osd_ev_prev[OSD_EVENT_BUF];
static size_t osd_ev_prev_len;
static int osd_prev_canvas_w, osd_prev_canvas_h;
static unsigned osd_prev_sub_generation;
static int osd_prev_sub_x, osd_prev_sub_y, osd_prev_sub_used;
static int osd_prev_full_canvas;
static int osd_force_rebuild = 1;

static SDL_Surface *osd_surface;
static int osd_surface_x, osd_surface_y;
static int osd_surface_valid;
static unsigned osd_generation = 1;

static int osd_canvas_h;
static double osd_play_w;

static double osd_ref_step, osd_ref_ink;

void format_time(char *buf, int bufsz, double secs) {
    int s = (int)secs;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sc = s % 60;
    snprintf(buf, bufsz, "%02d:%02d:%02d", h, m, sc);
}

static const char *osd_ui_family(void) {
    return osd_font_family[0] ? osd_font_family : LASS_UI_FAMILY;
}

const char *osd_font_name(void) {
    return osd_ui_family();
}

static double osd_at_least_px(double units, double px) {
    if (osd_canvas_h > 0) {
        double floor_units = px * OSD_RES_H / osd_canvas_h;

        if (floor_units > units) {
            return floor_units;
        }
    }

    return units;
}

static ASS_Style *osd_style_init(ASS_Track *track) {
    int sid = ass_alloc_style(track);
    ASS_Style *st;

    if (sid < 0) {
        return NULL;
    }
    track->default_style = sid;
    st = track->styles + sid;

    st->Name = lass_strdup("Default");
    st->FontName = lass_strdup(osd_ui_family());
    if (!st->Name || !st->FontName) {
        return NULL;
    }
    st->FontSize = OSD_FONT_SIZE;
    st->PrimaryColour = 0xFFFFFF00;
    st->SecondaryColour = 0xFFFFFF00;
    st->OutlineColour = 0x00000000;
    st->BackColour = 0x00000000;
    st->Outline = OSD_OUTLINE;
    st->Shadow = 0;
    st->Alignment = 7;
    st->ScaleX = 1.0;
    st->ScaleY = 1.0;
    st->BorderStyle = 1;
    st->Encoding = 1;

    return st;
}

static ASS_Track *osd_track_new(ASS_Library *lib, int res_x, int res_y) {
    ASS_Track *track = ass_new_track(lib);

    if (!track) {
        return NULL;
    }
    track->track_type = TRACK_TYPE_ASS;
    track->Timer = 100.0;
    track->PlayResX = res_x;
    track->PlayResY = res_y;
    track->WrapStyle = OSD_WRAP_LINE;
    track->Kerning = 1;
    track->ScaledBorderAndShadow = 1;
    if (!osd_style_init(track)) {
        ass_free_track(track);
        return NULL;
    }
    ass_track_set_feature(track, ASS_FEATURE_WRAP_UNICODE, 1);

    return track;
}

static int osd_engine_ensure(void) {
    if (osd_renderer && osd_measure_renderer && osd_track && osd_measure_track) {
        return 1;
    }
    if (osd_engine_failed) {
        return 0;
    }

    if (!osd_library) {
        osd_library = lass_library_new(0);
    }
    if (osd_library) {
        if (!osd_renderer && (osd_renderer = ass_renderer_init(osd_library))) {
            lass_renderer_setup(osd_renderer, osd_ui_family());
        }
        if (!osd_measure_renderer &&
            (osd_measure_renderer = ass_renderer_init(osd_library))) {
            lass_renderer_setup(osd_measure_renderer, osd_ui_family());
            ass_set_frame_size(osd_measure_renderer,
                               OSD_MEASURE_RES / OSD_MEASURE_SCALE,
                               OSD_MEASURE_RES / OSD_MEASURE_SCALE);
        }
        if (!osd_track) {
            osd_track = osd_track_new(osd_library, (int)OSD_RES_H, (int)OSD_RES_H);
        }
        if (!osd_measure_track) {
            osd_measure_track = osd_track_new(osd_library, OSD_MEASURE_RES,
                                              OSD_MEASURE_RES);
        }
    }

    if (!osd_renderer || !osd_measure_renderer || !osd_track ||
        !osd_measure_track) {
        log_warn("Could not initialize the libass renderer for the OSD.\n");
        osd_engine_failed = 1;
        return 0;
    }

    return 1;
}

static void osd_set_event(ASS_Track *track, const char *text, int margin_r,
                          int order) {
    int eid = ass_alloc_event(track);
    ASS_Event *e;

    if (eid < 0) {
        return;
    }
    e = track->events + eid;
    e->Start = 0;
    e->Duration = 1 << 30;
    e->Style = track->default_style;
    e->MarginR = margin_r;
    e->ReadOrder = order;
    e->Text = lass_strdup(text);
}

#define OSD_MEASURE_CACHE 64

typedef struct {
    uint64_t hash;
    double fs, wrap_w;
    OsdMetrics m;
    int wrap;
    unsigned valid : 1;
} OsdMeasureEntry;

static OsdMeasureEntry osd_measure_cache[OSD_MEASURE_CACHE];

static uint64_t osd_hash_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;

    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ull;
    }

    return h;
}

static uint64_t osd_hash(const char *body, double fs, double wrap_w, int wrap) {
    uint64_t h = 1469598103934665603ull;

    h = osd_hash_bytes(h, body, strlen(body));
    h = osd_hash_bytes(h, &fs, sizeof(fs));
    h = osd_hash_bytes(h, &wrap_w, sizeof(wrap_w));
    h = osd_hash_bytes(h, &wrap, sizeof(wrap));

    return h;
}

static void osd_measure_flush(void) {
    memset(osd_measure_cache, 0, sizeof(osd_measure_cache));
    osd_ref_step = osd_ref_ink = 0.0;
}

static int osd_measure_ink(const char *body, double fs, double wrap_w, int wrap,
                           double *out_w, double *out_h) {
    static char buf[OSD_BODY_MAX + 128];
    LassBounds b;
    ASS_Image *img;
    int margin_r = 0;
    int change;

    if (!osd_engine_ensure()) {
        return 0;
    }
    if (wrap_w > 0.0 && wrap_w < OSD_MEASURE_RES) {
        margin_r = (int)(OSD_MEASURE_RES - wrap_w + 0.5);
        if (margin_r < 1) {
            margin_r = 1;
        }
    }
    if (snprintf(buf, sizeof(buf),
                 "{\\an7\\pos(%d,%d)\\fs%.4f\\bord%.4f\\q%d}%s",
                 OSD_MEASURE_ORIGIN, OSD_MEASURE_ORIGIN, fs,
                 OSD_OUTLINE * fs / OSD_FONT_SIZE, wrap,
                 body) >= (int)sizeof(buf)) {
        return 0;
    }

    ass_flush_events(osd_measure_track);
    osd_set_event(osd_measure_track, buf, margin_r, 0);

    img = ass_render_frame(osd_measure_renderer, osd_measure_track, 0, &change);
    if (!img || !lass_ink_bounds(img, OSD_MEASURE_RES / OSD_MEASURE_SCALE, OSD_MEASURE_RES / OSD_MEASURE_SCALE, &b)) {
        *out_w = *out_h = 0.0;
        return 1;
    }
    *out_w = (double)(b.x1 - b.x0) * OSD_MEASURE_SCALE;
    *out_h = (double)(b.y1 - b.y0) * OSD_MEASURE_SCALE;

    return 1;
}

static int osd_reference(double fs, double *step, double *ink) {
    if (osd_ref_step <= 0.0) {
        double w, one = 0.0, two = 0.0, ref = 0.0;

        if (!osd_measure_ink("X", OSD_FONT_SIZE, 0.0, OSD_WRAP_NONE, &w, &one) ||
            !osd_measure_ink("X\\NX", OSD_FONT_SIZE, 0.0, OSD_WRAP_NONE, &w,
                             &two) ||
            !osd_measure_ink("Ag", OSD_FONT_SIZE, 0.0, OSD_WRAP_NONE, &w, &ref)) {
            return 0;
        }
        osd_ref_step = two - one;
        osd_ref_ink = ref;
        if (osd_ref_step <= 0.0) {
            osd_ref_step = OSD_FONT_SIZE * 1.2;
            osd_ref_ink = OSD_FONT_SIZE;
        }
    }
    *step = osd_ref_step * fs / OSD_FONT_SIZE;
    *ink = osd_ref_ink * fs / OSD_FONT_SIZE;

    return 1;
}

static int osd_measure(const char *body, double fs, double wrap_w, int wrap,
                       OsdMetrics *m) {
    uint64_t hash;
    OsdMeasureEntry *e;
    double ink_w = 0.0, ink_h = 0.0, step = 0.0, ink_ref = 0.0;

    if (fs < OSD_MIN_FONT) {
        fs = OSD_MIN_FONT;
    }
    hash = osd_hash(body, fs, wrap_w, wrap);
    e = &osd_measure_cache[hash % OSD_MEASURE_CACHE];
    if (e->valid && e->hash == hash && e->fs == fs && e->wrap_w == wrap_w &&
        e->wrap == wrap) {
        *m = e->m;
        return 1;
    }

    if (!osd_reference(fs, &step, &ink_ref) ||
        !osd_measure_ink(body, fs, wrap_w, wrap, &ink_w, &ink_h)) {
        return 0;
    }

    m->w = ink_w;
    if (ink_h <= 0.0) {
        m->lines = 0;
        m->h = 0.0;
    } else {
        m->lines = (int)((ink_h - ink_ref) / step + 0.5) + 1;
        if (m->lines < 1) {
            m->lines = 1;
        }
        m->h = m->lines * step;
    }

    e->hash = hash;
    e->fs = fs;
    e->wrap_w = wrap_w;
    e->wrap = wrap;
    e->m = *m;
    e->valid = 1;

    return 1;
}

static int osd_fits(const OsdMetrics *m, double box_w, double box_h) {
    if (box_w > 0.0 && m->w > box_w) {
        return 0;
    }
    if (box_h > 0.0 && m->h > box_h) {
        return 0;
    }

    return 1;
}

static double osd_fit(const char *body, double fs0, double box_w, double box_h,
                      int wrap, OsdMetrics *out) {
    OsdMetrics m;
    double lo = 0.0, hi = fs0, best = 0.0, guess = 1.0, cand;

    out->w = out->h = 0.0;
    out->lines = 0;

    if (!osd_measure(body, fs0, box_w, wrap, &m)) {
        double step = 0.0, ink = 0.0;

        out->h = osd_reference(fs0, &step, &ink) ? step : fs0;
        out->lines = 1;
        return fs0;
    }
    if (osd_fits(&m, box_w, box_h)) {
        *out = m;
        return fs0;
    }

    if (box_w > 0.0 && m.w > box_w) {
        guess = FFMIN(guess, box_w / m.w);
    }
    if (box_h > 0.0 && m.h > box_h) {
        guess = FFMIN(guess, box_h / m.h);
    }
    cand = FFMAX(fs0 * guess, OSD_MIN_FONT);

    for (int pass = 0; pass < OSD_FIT_PASSES; pass++) {
        if (!osd_measure(body, cand, box_w, wrap, &m)) {
            break;
        }
        if (osd_fits(&m, box_w, box_h)) {
            best = cand;
            lo = cand;
            *out = m;
        } else {
            hi = cand;
        }
        if (best > 0.0 && hi - lo < 0.5) {
            break;
        }
        cand = 0.5 * (lo + hi);
        if (cand < OSD_MIN_FONT) {
            cand = OSD_MIN_FONT;
            if (best > 0.0) {
                break;
            }
        }
    }

    if (best <= 0.0) {
        best = OSD_MIN_FONT;
        osd_measure(body, best, box_w, wrap, out);
    }

    return best;
}

static void osd_events_reset(void) {
    osd_ev_len = 0;
    osd_ev_count = 0;
}

static av_printf_format(2, 3) void osd_emit(int margin_r, const char *fmt, ...) {
    char *dst = osd_ev_buf + osd_ev_len;
    size_t avail = sizeof(osd_ev_buf) - osd_ev_len;
    va_list ap;
    int n;

    if (osd_ev_count >= OSD_EVENTS_MAX || avail < 2) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(dst, avail, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= avail) {
        *dst = '\0';
        return;
    }

    osd_ev_margin_r[osd_ev_count] = margin_r;
    osd_ev_off[osd_ev_count++] = osd_ev_len;
    osd_ev_len += (size_t)n + 1;
}

static void osd_emit_text(double x, double y, double fs, double wrap_w,
                          int wrap, const char *extra, const char *body) {
    int margin_r = 0;

    if (wrap_w > 0.0 && wrap_w < osd_play_w) {
        margin_r = (int)(osd_play_w - wrap_w + 0.5);
        if (margin_r < 1) {
            margin_r = 1;
        }
    }
    osd_emit(margin_r, "{\\an7\\pos(%.2f,%.2f)\\fs%.4f\\bord%.4f\\q%d%s}%s", x,
             y, fs, OSD_OUTLINE * fs / OSD_FONT_SIZE, wrap, extra ? extra : "",
             body);
}

static void osd_emit_rect(double x, double y, double w, double h, unsigned r,
                          unsigned g, unsigned b, unsigned opacity) {
    if (w <= 0.0 || h <= 0.0) {
        return;
    }
    osd_emit(0,
             "{\\an7\\pos(%.2f,%.2f)\\bord0\\shad0\\1c&H%02X%02X%02X&"
             "\\1a&H%02X&\\p1}m 0 0 l %.2f 0 %.2f %.2f 0 %.2f{\\p0}",
             x, y, b, g, r, 255 - opacity, w, w, h, h);
}

static double osd_display_pos(VideoState *is) {
    return playhead_elapsed(is, effective_playhead(is));
}

static int has_active_subtitle(VideoState *is) {
    if (!is->subtitle_st) {
        return 0;
    }
    return subtitles_visible_at(effective_playhead(is));
}

static void osd_layout_init(OsdLayout *L) {
    L->w = osd_play_w;
    L->mx = 25.0;
    L->my = 22.0;
    L->stack_gap = 10.0;
    L->line_gap = 8.0;
    L->row_gap = 12.0;
    L->topleft_w = L->w - 2 * L->mx;
    if (L->topleft_w < 1.0) {
        L->topleft_w = 1.0;
    }
    L->topleft_cur = L->my;
    L->sub_bottom = OSD_RES_H / 12.0;
    L->subs_top = OSD_RES_H;
    L->seek_bar_y = OSD_RES_H * 3.0 / 4.0;
    L->seek_label_h = 0.0;
    if (osd_ref_step > 0.0 || osd_engine_ensure()) {
        double step = 0.0, ink = 0.0;

        if (osd_reference(OSD_FONT_SIZE, &step, &ink)) {
            L->seek_label_h = step;
        }
    }
}

static OsdRect osd_stack_remaining(const OsdLayout *L, double floor_y) {
    OsdRect b = {L->mx, L->topleft_cur, L->topleft_w,
                 FFMAX(1.0, floor_y - L->topleft_cur)};

    return b;
}

static void osd_stack_advance(OsdLayout *L, double consumed_h) {
    L->topleft_cur += consumed_h + L->stack_gap;
}

static const char *osd_body(const char *text) {
    static char escaped[OSD_BODY_MAX];
    static char routed[OSD_BODY_MAX];

    if (!lass_escape(escaped, sizeof(escaped), text)) {
        return NULL;
    }
    if (lass_route_emoji(escaped, osd_ui_family(), NULL, NULL, routed,
                         sizeof(routed))) {
        return routed;
    }

    return escaped;
}

static void osd_draw_abloop(VideoState *is, OsdLayout *L) {
    char a_str[16], line[64];
    OsdRect b = osd_stack_remaining(L, OSD_RES_H - L->my);
    const char *body;
    OsdMetrics m;
    double fs;
    double a = playhead_elapsed(is, ab_loop_a);

    format_time(a_str, sizeof(a_str), isnan(a) || a < 0 ? 0.0 : a);
    snprintf(line, sizeof(line), "A-B loop  %s -> [set B]", a_str);
    if (!(body = osd_body(line))) {
        return;
    }

    fs = osd_fit(body, OSD_FONT_SIZE, b.w, 0.0, OSD_WRAP_NONE, &m);
    osd_emit_text(b.x, b.y, fs, 0.0, OSD_WRAP_NONE, NULL, body);
    osd_stack_advance(L, m.h);
}

static void osd_draw_message(OsdLayout *L) {
    OsdRect b = osd_stack_remaining(L, OSD_RES_H - L->my);
    const char *body;
    OsdMetrics m;
    double fs;

    if (!(body = osd_body(osd_message))) {
        return;
    }
    fs = osd_fit(body, OSD_FONT_SIZE, b.w, b.h, OSD_WRAP_LINE, &m);
    osd_emit_text(b.x, b.y, fs, b.w, OSD_WRAP_LINE, NULL, body);
    osd_stack_advance(L, m.h);
}

static void osd_draw_status(VideoState *is, OsdLayout *L) {
    double pos = osd_display_pos(is);
    double dur = playhead_length(is);
    char pos_str[16], dur_str[16], line[64], body[512];
    const char *sym;
    OsdRect b = osd_stack_remaining(L, OSD_RES_H - L->my);
    OsdMetrics m;
    double fs, consumed;
    int pct;

    format_time(pos_str, sizeof(pos_str), isnan(pos) || pos < 0 ? 0.0 : pos);
    format_time(dur_str, sizeof(dur_str), dur);
    pct = (dur > 0 && !isnan(pos) && pos >= 0) ? (int)(100.0 * pos / dur + 0.5)
                                               : 0;
    sym = is->paused ? "\xEE\x80\x82" : "\xEE\x80\x81";
    snprintf(line, sizeof(line), "%s / %s (%d%%)", pos_str, dur_str, pct);

    snprintf(body, sizeof(body), "{\\fn" LASS_SYMBOL_FAMILY "}%s{\\fn%s} %s",
             sym, osd_ui_family(), line);

    fs = osd_fit(body, OSD_FONT_SIZE, b.w, 0.0, OSD_WRAP_NONE, &m);
    osd_emit_text(b.x, b.y, fs, 0.0, OSD_WRAP_NONE, NULL, body);
    consumed = m.h;

    if (dur > 0) {
        double frac = (!isnan(pos) && pos >= 0) ? FFMIN(pos / dur, 1.0) : 0.0;
        double bar_w = m.w;
        double bar_h = osd_at_least_px(6.0, 3.0);
        double pad = FFMAX(osd_at_least_px(1.0, 1.0), bar_h / 3.0);
        double bar_x = b.x;
        double bar_y = b.y + m.h + L->line_gap + pad;
        double filled = bar_w * frac;

        osd_emit_rect(bar_x - pad, bar_y - pad, bar_w + 2 * pad, bar_h + 2 * pad,
                      80, 80, 80, 200);
        osd_emit_rect(bar_x, bar_y, filled, bar_h, 210, 210, 210, 255);
        consumed = m.h + L->line_gap + pad + bar_h + pad;
    }

    osd_stack_advance(L, consumed);
}

static double osd_sub_reserved_top(const OsdLayout *L) {
    double top = OSD_RES_H - OSD_RES_H * OSD_SUB_RESERVE_RATIO - L->sub_bottom;

    return top > 0.0 ? top : 0.0;
}

static int osd_text_subtitles_present(const VideoState *is) {
    const AVCodecDescriptor *d;

    if (!is->subtitle_st || !is->subtitle_st->codecpar) {
        return 0;
    }
    d = avcodec_descriptor_get(is->subtitle_st->codecpar->codec_id);

    return d && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

static void osd_draw_info(VideoState *is, OsdLayout *L) {
    static char info[OSD_INFO_TEXT_MAX];
    OsdInfoProvider provider =
        osd_info_page == 2 ? osd_stats_provider : osd_info_provider;
    const char *body;
    OsdRect box;
    OsdMetrics m;
    double floor_y, min_floor, fs;

    info[0] = '\0';
    if (provider) {
        provider(is, info, sizeof(info));
    }
    if (!info[0] || !(body = osd_body(info))) {
        return;
    }

    if (osd_text_subtitles_present(is)) {
        floor_y = FFMIN(osd_sub_reserved_top(L), L->subs_top) - L->line_gap;
    } else if (L->subs_top < OSD_RES_H) {
        floor_y = L->subs_top - L->line_gap;
    } else {
        floor_y = OSD_RES_H - L->my;
    }
    min_floor = L->my + FFMAX(1.0, (OSD_RES_H - 2 * L->my) / 2.0);
    if (floor_y < min_floor) {
        floor_y = min_floor;
    }

    box.x = L->mx;
    box.y = L->my;
    box.w = FFMAX(1.0, L->w - L->mx - L->line_gap);
    box.h = FFMAX(1.0, floor_y - L->my);

    fs = osd_fit(body, OSD_FONT_SIZE * OSD_INFO_SIZE_RATIO, box.w, box.h,
                 OSD_WRAP_LINE, &m);
    osd_emit_text(box.x, box.y, fs, box.w, OSD_WRAP_LINE, NULL, body);
}

static void osd_draw_delete(OsdLayout *L) {
    OsdRect box = {L->mx, L->my, L->topleft_w, OSD_RES_H - 2 * L->my};
    const char *body;
    OsdMetrics m;
    double fs;

    if (!(body = osd_body(osd_delete_prompt))) {
        return;
    }
    fs = osd_fit(body, OSD_FONT_SIZE, box.w, box.h, OSD_WRAP_LINE, &m);
    osd_emit_text(box.x, box.y, fs, box.w, OSD_WRAP_LINE, NULL, body);
}

static void osd_draw_volume(VideoState *is, OsdLayout *L) {
    int vol_pct = (int)(100.0 * is->audio_volume / FFP_MIX_MAXVOLUME + 0.5);
    int vol_max = is->audio_volume_max > 0 ? is->audio_volume_max
                                           : FFP_MIX_MAXVOLUME;
    int boosted = vol_max > FFP_MIX_MAXVOLUME;
    double bar_w = L->w / 4.0;
    double bar_h = osd_at_least_px(8.0, 3.0);
    double bar_pad = FFMAX(osd_at_least_px(1.0, 1.0), bar_h / 2.0);
    double bar_x = (L->w - bar_w) / 2.0;
    double seek_stack_top = L->seek_bar_y - L->seek_label_h - L->line_gap;
    double bar_y = seek_stack_top - 10.0 - (bar_h / 2.0 + bar_pad);
    double unity = boosted ? bar_w * FFP_MIX_MAXVOLUME / vol_max : bar_w;
    char label[32], body[512];
    const char *sym;
    OsdMetrics m;
    double fs, row_x, row_y;

    osd_emit_rect(bar_x - bar_pad, bar_y - bar_h / 2.0 - bar_pad,
                  bar_w + 2 * bar_pad, bar_h + 2 * bar_pad, 80, 80, 80, 200);

    if (vol_pct > 0 && !is->muted) {
        double filled = FFMIN(bar_w * is->audio_volume / vol_max, bar_w);
        double base = FFMIN(filled, unity);

        osd_emit_rect(bar_x, bar_y - bar_h / 2.0, base, bar_h, 210, 210, 210,
                      255);
        if (filled > unity) {
            osd_emit_rect(bar_x + unity, bar_y - bar_h / 2.0, filled - unity,
                          bar_h, 230, 150, 60, 255);
        }
    }
    if (boosted) {
        osd_emit_rect(bar_x + unity - 1.0, bar_y - bar_h / 2.0 - bar_pad / 2.0,
                      2.0, bar_h + bar_pad, 255, 255, 255, 230);
    }

    if (is->muted) {
        sym = "\xEE\x84\x8A";
    } else if (vol_pct <= 33) {
        sym = "\xEE\x84\x8B";
    } else if (vol_pct <= 66) {
        sym = "\xEE\x84\x8C";
    } else {
        sym = "\xEE\x84\x8D";
    }
    snprintf(label, sizeof(label), "%d%%", vol_pct);
    snprintf(body, sizeof(body), "{\\fn" LASS_SYMBOL_FAMILY "}%s{\\fn%s} %s",
             sym, osd_ui_family(), label);

    fs = osd_fit(body, OSD_FONT_SIZE, L->w - 2 * L->mx, 0.0, OSD_WRAP_NONE, &m);
    row_y = bar_y - m.h - L->row_gap;
    row_x = bar_x;
    if (row_x + m.w > L->w - L->mx) {
        row_x = L->w - L->mx - m.w;
    }
    if (row_x < L->mx) {
        row_x = L->mx;
    }
    osd_emit_text(row_x, row_y, fs, 0.0, OSD_WRAP_NONE,
                  is->muted ? "\\1c&HD2D2D2&" : NULL, body);
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

static void osd_build(VideoState *is, int ch, double subs_top_px) {
    OsdVis v = osd_resolve(is);
    OsdLayout L;

    osd_events_reset();
    osd_layout_init(&L);
    if (subs_top_px >= 0.0 && ch > 0) {
        L.subs_top = subs_top_px * OSD_RES_H / ch;
    }

    if (v.del) {
        osd_draw_delete(&L);
        return;
    }
    if (v.info) {
        osd_draw_info(is, &L);
        return;
    }

    if (v.ab_loop) {
        osd_draw_abloop(is, &L);
    }
    if (v.message) {
        osd_draw_message(&L);
    }
    if (v.status) {
        osd_draw_status(is, &L);
    }
    if (v.volume) {
        osd_draw_volume(is, &L);
    }
}

static int osd_track_ensure(int res_x) {
    if (osd_track && osd_track->PlayResX == res_x) {
        return 1;
    }
    ass_free_track(osd_track);
    osd_track = osd_track_new(osd_library, res_x, (int)OSD_RES_H);

    return osd_track != NULL;
}

static ASS_Image *osd_render(int cw, int ch) {
    int change;

    if (!osd_engine_ensure() || !osd_ev_count) {
        return NULL;
    }
    if (!osd_track_ensure((int)(osd_play_w + 0.5))) {
        return NULL;
    }

    ass_set_frame_size(osd_renderer, cw, ch);

    ass_flush_events(osd_track);
    for (int i = 0; i < osd_ev_count; i++) {
        osd_set_event(osd_track, osd_ev_buf + osd_ev_off[i], osd_ev_margin_r[i],
                      i);
    }

    return ass_render_frame(osd_renderer, osd_track, 0, &change);
}

static int osd_surface_ensure(int w, int h, SDL_PixelFormat format) {
    if (osd_surface && osd_surface->w == w && osd_surface->h == h &&
        osd_surface->format == format) {
        SDL_ClearSurface(osd_surface, 0.0f, 0.0f, 0.0f, 0.0f);
        return 1;
    }
    SDL_DestroySurface(osd_surface);
    osd_surface = SDL_CreateSurface(w, h, format);
    if (!osd_surface) {
        return 0;
    }
    SDL_ClearSurface(osd_surface, 0.0f, 0.0f, 0.0f, 0.0f);

    return 1;
}

static int osd_composite(VideoState *is, int cw, int ch, int full_canvas,
                         const SubtitleOverlay *ov) {
    ASS_Image *img;
    LassBounds b;
    int changed;

    osd_play_w = ch > 0 ? OSD_RES_H * cw / ch : OSD_RES_H;
    if (osd_play_w < 16.0) {
        osd_play_w = 16.0;
    }
    osd_canvas_h = ch;

    osd_build(is, ch, ov && ov->surf ? ov->y : -1.0);

    changed = osd_force_rebuild || !osd_surface_valid ||
        osd_prev_canvas_w != cw || osd_prev_canvas_h != ch ||
        osd_prev_full_canvas != full_canvas ||
        osd_ev_prev_len != osd_ev_len ||
        memcmp(osd_ev_prev, osd_ev_buf, osd_ev_len) != 0;

    if (full_canvas) {
        int used = ov && ov->surf;

        if (osd_prev_sub_used != used ||
            (used && (osd_prev_sub_generation != ov->generation || osd_prev_sub_x != ov->x || osd_prev_sub_y != ov->y))) {
            changed = 1;
        }
        osd_prev_sub_used = used;
        if (used) {
            osd_prev_sub_generation = ov->generation;
            osd_prev_sub_x = ov->x;
            osd_prev_sub_y = ov->y;
        }
    }

    if (!changed) {
        return osd_surface_valid;
    }

    osd_force_rebuild = 0;
    osd_prev_canvas_w = cw;
    osd_prev_canvas_h = ch;
    osd_prev_full_canvas = full_canvas;
    osd_ev_prev_len = osd_ev_len;
    memcpy(osd_ev_prev, osd_ev_buf, osd_ev_len);
    osd_surface_valid = 0;
    osd_generation++;

    img = osd_render(cw, ch);

    if (full_canvas) {
        if (!img && !(ov && ov->surf)) {
            return 0;
        }
        if (!osd_surface_ensure(cw, ch, SDL_PIXELFORMAT_ABGR8888)) {
            return 0;
        }
        if (ov && ov->surf) {
            lass_blit(osd_surface, ov->surf, ov->x, ov->y);
        }
        lass_blend(osd_surface, img, 0, 0);
        osd_surface_x = 0;
        osd_surface_y = 0;
    } else {
        if (!img || !lass_bounds(img, cw, ch, &b)) {
            return 0;
        }
        if (!osd_surface_ensure(b.x1 - b.x0, b.y1 - b.y0,
                                SDL_PIXELFORMAT_ARGB8888)) {
            return 0;
        }
        lass_blend(osd_surface, img, b.x0, b.y0);
        osd_surface_x = b.x0;
        osd_surface_y = b.y0;
    }

    osd_surface_valid = 1;

    return 1;
}

void osd_invalidate_textures(void) {
    osd_force_rebuild = 1;
}

static int osd_should_show(VideoState *is) {
    int64_t now = (int64_t)SDL_GetTicks();

    return now < osd_status_show_until || now < osd_volume_show_until ||
        now < osd_message_show_until || osd_info_sticky ||
        osd_delete_prompt_active || has_active_subtitle(is) ||
        ab_loop_defining();
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

static int osd_subtitle_overlay(VideoState *is, int cw, int ch,
                                SubtitleOverlay *ov) {
    SDL_Rect vr = is->render_params.target_rect;

    if (!has_active_subtitle(is)) {
        return 0;
    }

    return subtitles_render(is, cw, ch, &vr, effective_playhead(is), ov) &&
        ov->surf;
}

/* Composite the OSD into a surface for the renderer to blend as an overlay. */
void osd_prepare(VideoState *is) {
    SubtitleOverlay ov = {0};
    int have_ov;
    int cw, ch;

    if (!renderer || !osd_should_show(is) || !osd_engine_ensure()) {
        return;
    }

    osd_canvas_size(is, &cw, &ch);
    if (cw <= 0 || ch <= 0) {
        return;
    }

    have_ov = osd_subtitle_overlay(is, cw, ch, &ov);
    if (!osd_composite(is, cw, ch, 1, have_ov ? &ov : NULL)) {
        return;
    }

    is->render_params.osd_pixels = osd_surface->pixels;
    is->render_params.osd_width = osd_surface->w;
    is->render_params.osd_height = osd_surface->h;
    is->render_params.osd_stride = osd_surface->pitch;
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
    osd_force_rebuild = 1;
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

void osd_init(void) {
    const char *env = getenv("LACHESIS_OSD_FONT");
    size_t n = 0;

    if (!env || !env[0]) {
        return;
    }
    if (strchr(env, '/') || strchr(env, '\\')) {
        log_warn("LACHESIS_OSD_FONT expects a font family instead of a file.\n");
    }
    for (const char *p = env; *p && n + 1 < sizeof(osd_font_family); p++) {
        if (*p != '{' && *p != '}' && *p != '\\') {
            osd_font_family[n++] = *p;
        }
    }
    osd_font_family[n] = '\0';
}

void osd_warmup(void) {
    double step = 0, ink = 0;
    int w = 0, h = 0;

    if (!osd_engine_ensure()) {
        return;
    }

    osd_reference(OSD_FONT_SIZE, &step, &ink);

    if (window) {
        SDL_GetWindowSizeInPixels(window, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        w = default_width;
        h = default_height;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    osd_canvas_h = h;
    osd_play_w = OSD_RES_H * w / h;
    if (osd_track_ensure((int)(osd_play_w + 0.5))) {
        int change;

        ass_set_frame_size(osd_renderer, w, h);
        ass_flush_events(osd_track);
        osd_set_event(osd_track,
                      "{\\an7\\pos(0,0)\\fs66\\bord2\\q2}"
                      "0123456789:/%()., "
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "abcdefghijklmnopqrstuvwxyz"
                      "{\\fn" LASS_SYMBOL_FAMILY "}"
                      "\xEE\x80\x81\xEE\x80\x82\xEE\x84\x8A"
                      "\xEE\x84\x8B\xEE\x84\x8C\xEE\x84\x8D"
                      "{\\fn" LASS_EMOJI_FAMILY "}\xF0\x9F\x98\x80",
                      0, 0);
        (void)ass_render_frame(osd_renderer, osd_track, 0, &change);
        ass_flush_events(osd_track);
    }

    osd_force_rebuild = 1;
}

void osd_uninit(void) {
    SDL_DestroySurface(osd_surface);
    osd_surface = NULL;
    osd_surface_valid = 0;

    if (osd_track) {
        ass_free_track(osd_track);
        osd_track = NULL;
    }
    if (osd_measure_track) {
        ass_free_track(osd_measure_track);
        osd_measure_track = NULL;
    }
    if (osd_renderer) {
        ass_renderer_done(osd_renderer);
        osd_renderer = NULL;
    }
    if (osd_measure_renderer) {
        ass_renderer_done(osd_measure_renderer);
        osd_measure_renderer = NULL;
    }
    lass_library_free(osd_library);
    osd_library = NULL;
    osd_measure_flush();
    osd_engine_failed = 0;
    osd_force_rebuild = 1;
}
