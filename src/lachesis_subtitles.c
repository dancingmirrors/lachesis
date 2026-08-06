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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <ass/ass.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/macros.h>

#include <SDL3/SDL.h>

#include "lachesis_ass.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_subtitles.h"

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

static void ass_engine_uninit_locked(void) {
    if (ass_track) {
        ass_free_track(ass_track);
        ass_track = NULL;
    }
    if (ass_renderer) {
        ass_renderer_done(ass_renderer);
        ass_renderer = NULL;
    }
    lass_library_free(ass_library);
    ass_library = NULL;
    if (ass_surface) {
        SDL_DestroySurface(ass_surface);
        ass_surface = NULL;
    }
    ass_frame_w = ass_frame_h = 0;
    ass_storage_w = ass_storage_h = 0;
}

static int ass_engine_init_locked(void) {
    if (ass_renderer) {
        return 0;
    }

    if (!ass_library) {
        ass_library = lass_library_new(1);
        if (!ass_library) {
            return -1;
        }
    }

    ass_renderer = ass_renderer_init(ass_library);
    if (!ass_renderer) {
        log_warn("Could not initialize the libass renderer.\n");
        return -1;
    }

    lass_renderer_setup(ass_renderer, LASS_UI_FAMILY);

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

static const char *ass_style_font_locked(const char *name, size_t len,
                                         void *ctx) {
    (void)ctx;

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

/* ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text */
static int ass_route_emoji_locked(const char *in, char *out, size_t outsz) {
    const char *style = NULL, *text = NULL;
    const char *cur_font;
    size_t style_len = 0, head;
    int fields = 0;

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

    cur_font = ass_style_font_locked(style, style_len, NULL);
    if (!cur_font || !*cur_font) {
        return 0;
    }

    head = (size_t)(text - in);
    if (head >= outsz) {
        return 0;
    }
    memcpy(out, in, head);

    if (!lass_route_emoji(text, cur_font, ass_style_font_locked, NULL,
                          out + head, outsz - head)) {
        return 0;
    }

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

    ass_process_codec_private(ass_track,
                              avctx->subtitle_header
                                  ? (char *)avctx->subtitle_header
                                  : "",
                              avctx->subtitle_header_size > 0
                                  ? avctx->subtitle_header_size
                                  : 0);

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

static int ass_composite_locked(ASS_Image *img, int frame_w, int frame_h) {
    LassBounds b;

    if (!lass_bounds(img, frame_w, frame_h, &b)) {
        if (ass_surface) {
            SDL_DestroySurface(ass_surface);
            ass_surface = NULL;
        }
        return 0;
    }

    if (ass_surface) {
        SDL_DestroySurface(ass_surface);
    }
    ass_surface = SDL_CreateSurface(b.x1 - b.x0, b.y1 - b.y0,
                                    SDL_PIXELFORMAT_ARGB8888);
    if (!ass_surface) {
        return 0;
    }
    SDL_ClearSurface(ass_surface, 0.0f, 0.0f, 0.0f, 0.0f);

    lass_blend(ass_surface, img, b.x0, b.y0);

    ass_surface_x = b.x0;
    ass_surface_y = b.y0;

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

    if (is && is->render_storage_w > 0 && is->render_storage_h > 0) {
        storage_w = is->render_storage_w;
        storage_h = is->render_storage_h;
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
    if (geometry_changed) {
        log_verbose("libass: frame %dx%d, storage %dx%d.\n", frame_w,
                    frame_h, storage_w, storage_h);
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
        SDL_SetAtomicInt(&is->sub_read_thread_done, 1);
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
    SDL_SetAtomicInt(&is->sub_read_thread_done, 1);

    return 0;
}

static int sub_is_regular_file(const char *path) {
#ifdef _WIN32
    wchar_t *wpath;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    DWORD attr;

    if (n <= 0) {
        return 0;
    }
    wpath = av_malloc_array((size_t)n, sizeof(*wpath));
    if (!wpath) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n) <= 0) {
        av_free(wpath);
        return 0;
    }
    attr = GetFileAttributesW(wpath);
    av_free(wpath);

    return attr != INVALID_FILE_ATTRIBUTES &&
        !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int external_subtitle_open(VideoState *is) {
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
#ifdef _WIN32
    const char *bslash = strrchr(is->filename, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
#endif
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    size_t base_len = dot ? (size_t)(dot - is->filename) : strlen(is->filename);

    char path[4096];
    int found = 0;
    for (int i = 0; sub_exts[i]; i++) {
        snprintf(path, sizeof(path), "%.*s.%s", (int)base_len, is->filename, sub_exts[i]);
        if (sub_is_regular_file(path)) {
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

    SDL_SetAtomicInt(&is->sub_read_thread_done, 0);
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

int open_external_subtitle(VideoState *is) {
    int ret;

    if (!pipeline_setup_begin(is)) {
        return AVERROR_EXIT;
    }
    ret = external_subtitle_open(is);
    pipeline_setup_end(is);

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
