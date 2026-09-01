/*
 * Copyright © 2003 Fabrice Bellard
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

#include "lachesis_config.h"
#include "version.h"

#include <libplacebo/config.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/fifo.h>
#include <libavutil/film_grain_params.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/tx.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_mutex.h>

#include <stdbool.h>
#include <strings.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
/* clang-format off */
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
/* clang-format on */
#undef main /* We don't want SDL to override our main(). */
#define PATH_SEPARATOR '\\'
#else
#include <unistd.h>
#define PATH_SEPARATOR '/'
#endif

#include "lachesis_alloc.h"
#include "lachesis_archive.h"
#include "lachesis_audio.h"
#include "lachesis_degrade.h"
#include "lachesis_deinterlace.h"
#include "lachesis_demux.h"
#include "lachesis_equalizer.h"
#include "lachesis_filters.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_interpolate.h"
#include "lachesis_keys.h"
#include "lachesis_log.h"
#include "lachesis_network.h"
#include "lachesis_normalize.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_playlist.h"
#include "lachesis_present.h"
#include "lachesis_rc.h"
#include "lachesis_renderer.h"
#include "lachesis_screenshot.h"
#include "lachesis_single.h"
#include "lachesis_subtitles.h"
#include "lachesis_terminal.h"
#include "lachesis_view360.h"

const char program_name[] = "lachesis";
const int program_birth_year = 2003;

static void init_dynload(void) {
#ifdef _WIN32
    /* Remove the current working directory from the DLL search path as a security precaution. */
    SetDllDirectoryW(L"");
#endif
}

#ifdef _WIN32
static int win32_handle_valid(HANDLE handle) {
    return handle && handle != INVALID_HANDLE_VALUE;
}

static void win32_attach_console(void) {
    static const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    static const char *const devices[] = {"CONIN$", "CONOUT$", "CONOUT$"};
    static const char *const modes[] = {"r", "w", "w"};
    FILE *streams[] = {stdin, stdout, stderr};
    HANDLE inherited[FF_ARRAY_ELEMS(ids)];

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ids); i++) {
        inherited[i] = GetStdHandle(ids[i]);
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS) &&
        GetLastError() != ERROR_ACCESS_DENIED) {
        return;
    }

    for (size_t i = 0; i < FF_ARRAY_ELEMS(ids); i++) {
        intptr_t handle;

        if (win32_handle_valid(inherited[i])) {
            SetStdHandle(ids[i], inherited[i]);
            continue;
        }
        if (!freopen(devices[i], modes[i], streams[i])) {
            continue;
        }
        handle = _get_osfhandle(_fileno(streams[i]));
        if (handle != -1 && handle != -2) {
            SetStdHandle(ids[i], (HANDLE)handle);
        }
    }
}
#endif

#ifdef _WIN32
static void win32_argv_to_utf8(int *argc_p, char ***argv_p) {
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        return;
    }
    char **uargv = av_calloc((size_t)wargc + 1, sizeof(*uargv));
    if (!uargv) {
        LocalFree(wargv);
        return;
    }
    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) {
            goto fail;
        }
        uargv[i] = av_malloc((size_t)n);
        if (!uargv[i]) {
            goto fail;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    for (int i = 0; i < wargc; i++) {
        alloc_track_disown(uargv[i]);
    }
    alloc_track_disown(uargv);
    *argc_p = wargc;
    *argv_p = uargv;
    return;

fail:
    for (int i = 0; i < wargc; i++) {
        av_free(uargv[i]);
    }
    av_free(uargv);
    LocalFree(wargv);
}
#endif

#define EXACT_SEEK_SLACK 0.005
#define EXACT_SEEK_MAX_RUNUP 30.0
#define EXACT_SEEK_BACKOFF 0.5
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define AV_SYNC_THRESHOLD_MAX 0.1

#define AV_SYNC_SLEW_GAIN 0.1
#define AV_SYNC_SLEW_FACTOR 0.1
#define AV_SYNC_RESYNC_THRESHOLD 0.2
#define AV_SYNC_MAX_HOLD 0.5

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define REFRESH_RATE 0.01

#define SLOW_OPEN_US (500 * 1000)

#define OSD_ONLY_REFRESH_RATE (1.0 / 30.0)

#define REPLACE_WINDOW_MIN_US (1000 * 1000)

#define WINDOW_INSIST_US (2000 * 1000)

#define WINDOW_INSIST_TRIES 3

#define REMAP_CHECK_US (1000 * 1000)

#define RAISE_GRACE_US (300 * 1000)

#define CURSOR_HIDE_DELAY 1000000

#define AUDIO_START_MAX_WAIT_US (10 * 1000000)

#define USE_ONEPASS_SUBTITLE_RENDER 1

static const char *input_filename;

static char *window_title_path;
static char *window_title_shown;

static int display_deferred;

static int window_placed;

static int raise_wanted;
static int raise_asked;
static char *raise_token;
static int64_t raise_started_us;

static int pause_next_stream = 0;

static char **input_args = NULL;
static int n_input_args = 0;

int default_width = 640;
int default_height = 480;
int screen_width = 0;
int screen_height = 0;

#define VIEW_ZOOM_STEP 1.1f
#define VIEW_ZOOM_MIN 0.05f
#define VIEW_ZOOM_MAX 8.0f

static float view_zoom_want;
static float view_zoom_plain;
static float view_center_s = 0.5f;
static float view_center_t = 0.5f;
static float display_scale = 1.0f;
static float display_pan_x;
static float display_pan_y;

int lachesis_quiet;
int64_t cursor_last_shown;
int cursor_hidden = 0;
int frame_interpolation = 0;
int fatal_error_pending = 0;
int exit_status = 0;
enum View360Layout view360_layout = VIEW360_LAYOUT_OFF;
enum View360Projection view360_projection = VIEW360_PROJECTION_PANINI;
float sbs360_yaw = 0.0f;
float sbs360_pitch = VIEW360_DEFAULT_PITCH;
float sbs360_roll = 0.0f;
float sbs360_hfov = VIEW360_DEFAULT_HFOV;

void sbs360_reset_view(void) {
    sbs360_yaw = view360_default_yaw(view360_layout);
    sbs360_pitch = VIEW360_DEFAULT_PITCH;
    sbs360_roll = 0.0f;
    sbs360_hfov = view360_default_hfov(view360_projection);
}

SDL_Window *window;

Renderer *renderer;

#define RENDER_FAULT_LIMIT 8
#define RENDER_FAULT_LIMIT_LATE 90
static int render_fail_streak;
static int render_ever_ok;
static int render_fault_event_sent;

double ab_loop_a = LACHESIS_NAN;
double ab_loop_b = LACHESIS_NAN;

int ab_loop_defining(void) {
    return !isnan(ab_loop_a) && isnan(ab_loop_b);
}

double playback_speed = 1.0;

#define PLAYBACK_SPEED_MIN 0.2
#define PLAYBACK_SPEED_MAX 2.0

void thread_set_priority(SDL_ThreadPriority priority, const char *who) {
    if (SDL_SetCurrentThreadPriority(priority)) {
        return;
    }
    log_verbose("Couldn't set the %s thread priority: %s\n", who, SDL_GetError());
    SDL_ClearError();
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request) {
        return -1;
    }

    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0) {
        return ret;
    }
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;

    SDL_SignalCondition(q->cond);

    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0) {
        av_packet_free(&pkt1);
    }

    return ret;
}

int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index) {
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list) {
        return AVERROR(ENOMEM);
    }
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCondition();
    if (!q->cond) {
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;

    return 0;
}

void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList pkt1;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
        av_packet_free(&pkt1.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCondition(q->cond);
}

static void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_SignalCondition(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial) {
                *serial = pkt1.serial;
            }
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_WaitCondition(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);

    return ret;
}

int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_Condition *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt) {
        return AVERROR(ENOMEM);
    }
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    d->exact_done_serial = -1;
    d->exact_dropped_serial = -1;

    return 0;
}

int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    d->wait_us = 0;

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) {
                    return -1;
                }

                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    av_assert0(frame);
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        frame->pts = frame->best_effort_timestamp;
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    av_assert0(frame);
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        AVRational tb = (AVRational){1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE) {
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        } else if (d->next_pts != AV_NOPTS_VALUE) {
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        }
                        if (frame->pts != AV_NOPTS_VALUE) {
                            d->next_pts = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb;
                        }
                    }
                    break;
                default:
                    break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0) {
                    return 1;
                }
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0) {
                SDL_SignalCondition(d->empty_queue_cond);
            }
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                int64_t wait_t0 = av_gettime_relative();
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0) {
                    return -1;
                }
                d->wait_us += av_gettime_relative() - wait_t0;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial) {
                break;
            }
            av_packet_unref(d->pkt);
        } while (1);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !d->pkt->data) {
                    d->packet_pending = 1;
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        } else {
            if (d->pkt->buf && !d->pkt->opaque_ref) {
                FrameData *fd;

                d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!d->pkt->opaque_ref) {
                    return AVERROR(ENOMEM);
                }
                fd = (FrameData *)d->pkt->opaque_ref->data;
                fd->pkt_pos = d->pkt->pos;
            }

            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                d->packet_pending = 1;
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}

void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) {
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCondition())) {
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++) {
        if (!(f->queue[i].frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void frame_queue_destroy(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCondition(f->cond);
}

static void frame_queue_signal(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    SDL_SignalCondition(f->cond);
    SDL_UnlockMutex(f->mutex);
}

Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

Frame *frame_queue_peek_writable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size && !f->pktq->abort_request) {
        SDL_WaitCondition(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        return NULL;
    }

    return &f->queue[f->windex];
}

Frame *frame_queue_peek_readable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        SDL_WaitCondition(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        return NULL;
    }

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size) {
        f->windex = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_SignalCondition(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size) {
        f->rindex = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_SignalCondition(f->cond);
    SDL_UnlockMutex(f->mutex);
}

int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

int64_t frame_queue_last_pos(FrameQueue *f) {
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial) {
        return fp->pos;
    } else {
        return -1;
    }
}

void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

typedef struct DisplaySizes {
    int64_t fit_w, fit_h;
    int64_t nat_w, nat_h;
    int64_t base_w, base_h;
} DisplaySizes;

static void display_sizes(int scr_width, int scr_height, int pic_width,
                          int pic_height, AVRational pic_sar,
                          DisplaySizes *out) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height;

    if (pic_width < 1) {
        pic_width = 1;
    }
    if (pic_height < 1) {
        pic_height = 1;
    }
    scr_width = FFMAX(scr_width, 1);
    scr_height = FFMAX(scr_height, 1);

    if (video_rotate == 90 || video_rotate == 270) {
        int tmp = pic_width;
        pic_width = pic_height;
        pic_height = tmp;
        if (aspect_ratio.num > 0 && aspect_ratio.den > 0) {
            aspect_ratio = av_make_q(aspect_ratio.den, aspect_ratio.num);
        }
    }

    if (aspect_ratio.num <= 0 || aspect_ratio.den <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    out->fit_w = FFMAX(width, 1);
    out->fit_h = FFMAX(height, 1);

    height = pic_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    out->nat_w = FFMAX(width, 1);
    out->nat_h = FFMAX(height, 1);

    if (video_fill || out->nat_w > out->fit_w || out->nat_h > out->fit_h) {
        out->base_w = out->fit_w;
        out->base_h = out->fit_h;
    } else {
        out->base_w = out->nat_w;
        out->base_h = out->nat_h;
    }
}

static void view_center_to_display(float *u, float *v) {
    switch (video_rotate) {
    case 90:
        *u = 1.0f - view_center_t;
        *v = view_center_s;
        break;
    case 180:
        *u = 1.0f - view_center_s;
        *v = 1.0f - view_center_t;
        break;
    case 270:
        *u = view_center_t;
        *v = 1.0f - view_center_s;
        break;
    default:
        *u = view_center_s;
        *v = view_center_t;
        break;
    }
}

static void view_center_from_display(float u, float v) {
    switch (video_rotate) {
    case 90:
        view_center_s = v;
        view_center_t = 1.0f - u;
        break;
    case 180:
        view_center_s = 1.0f - u;
        view_center_t = 1.0f - v;
        break;
    case 270:
        view_center_s = 1.0f - v;
        view_center_t = u;
        break;
    default:
        view_center_s = u;
        view_center_t = v;
        break;
    }
}

static void view_pan_limits(int64_t width, int64_t height, int64_t clip_w,
                            int64_t clip_h, float *max_x, float *max_y) {
    *max_x = (float)(FFABS(width - clip_w) / 2);
    *max_y = (float)(FFABS(height - clip_h) / 2);
}

static void view_log(int scr_width, int scr_height, const DisplaySizes *sizes,
                     int64_t width, int64_t height) {
    static int64_t last[8];
    int64_t now[8] = {is_fullscreen, video_rotate, scr_width, scr_height,
                      sizes->nat_w, sizes->nat_h, width, height};

    if (!memcmp(now, last, sizeof(now))) {
        return;
    }
    memcpy(last, now, sizeof(now));

    log_verbose("View: %s %dx%d, box %dx%d, picture %dx%d at %.0f%% of the fit "
                "and %.0f%% of native, showing %.0f%% by %.0f%%.\n",
                is_fullscreen ? "screen" : "window", scr_width, scr_height,
                (int)sizes->fit_w, (int)sizes->fit_h, (int)width, (int)height,
                (double)((float)width / (float)sizes->fit_w * 100.0f),
                (double)((float)width / (float)sizes->nat_w * 100.0f),
                (double)(FFMIN((float)sizes->fit_w / (float)width, 1.0f) * 100.0f),
                (double)(FFMIN((float)sizes->fit_h / (float)height, 1.0f) * 100.0f));
}

static float view_zoom_settled(float plain) {
    if (view_zoom_want <= 0.0f) {
        return plain;
    }
    if (view_zoom_plain > 0.0f && view_zoom_want < view_zoom_plain) {
        return view_zoom_want / view_zoom_plain * plain;
    }

    return FFMAX(view_zoom_want, plain);
}

void calculate_display_rect(SDL_Rect *rect, SDL_Rect *clip, SDL_Rect *plain,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar) {
    DisplaySizes sizes;
    int64_t width, height, x, y;
    float max_pan_x, max_pan_y, u, v;

    scr_width = FFMAX(scr_width, 1);
    scr_height = FFMAX(scr_height, 1);
    display_sizes(scr_width, scr_height, pic_width, pic_height, pic_sar, &sizes);

    if (clip) {
        clip->x = scr_xleft + (int)((scr_width - sizes.fit_w) / 2);
        clip->y = scr_ytop + (int)((scr_height - sizes.fit_h) / 2);
        clip->w = (int)sizes.fit_w;
        clip->h = (int)sizes.fit_h;
    }
    if (plain) {
        plain->x = scr_xleft + (int)((scr_width - sizes.base_w) / 2);
        plain->y = scr_ytop + (int)((scr_height - sizes.base_h) / 2);
        plain->w = (int)sizes.base_w;
        plain->h = (int)sizes.base_h;
    }

    if (view_zoom_want > 0.0f) {
        float zoom = view_zoom_settled((float)sizes.base_w / (float)sizes.fit_w);

        width = FFMAX((int64_t)((float)sizes.fit_w * zoom), 1);
        height = FFMAX((int64_t)((float)sizes.fit_h * zoom), 1);
    } else {
        width = sizes.base_w;
        height = sizes.base_h;
    }
    display_scale = (float)width / (float)sizes.fit_w;
    view_log(scr_width, scr_height, &sizes, width, height);

    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;

    view_center_to_display(&u, &v);
    display_pan_x = (float)width * (0.5f - u);
    display_pan_y = (float)height * (0.5f - v);
    view_pan_limits(width, height, sizes.fit_w, sizes.fit_h, &max_pan_x, &max_pan_y);
    display_pan_x = av_clipf(display_pan_x, -max_pan_x, max_pan_x);
    display_pan_y = av_clipf(display_pan_y, -max_pan_y, max_pan_y);
    x += lrintf(display_pan_x);
    y += lrintf(display_pan_y);

    rect->x = scr_xleft + (int)x;
    rect->y = scr_ytop + (int)y;
    rect->w = FFMAX((int)width, 1);
    rect->h = FFMAX((int)height, 1);
}

static int view_picture(VideoState *is, Frame **out) {
    Frame *vp;

    if (!is->video_st || is->width <= 0 || is->height <= 0) {
        return 0;
    }
    vp = frame_queue_peek_last(&is->pictq);
    if (vp->width <= 0 || vp->height <= 0) {
        return 0;
    }
    *out = vp;

    return 1;
}

static float view_measure(VideoState *is, const Frame *vp, SDL_Rect *rect,
                          SDL_Rect *clip) {
    SDL_Rect unwanted;

    calculate_display_rect(rect ? rect : &unwanted, clip, NULL, 0, 0, is->width,
                           is->height, vp->width, vp->height, vp->sar);

    return display_scale;
}

static float view_default_zoom(const VideoState *is, const Frame *vp) {
    DisplaySizes sizes;

    display_sizes(is->width, is->height, vp->width, vp->height, vp->sar, &sizes);

    return (float)sizes.base_w / (float)sizes.fit_w;
}

float view_zoom_step(VideoState *is, int direction) {
    Frame *vp;
    float settled, want, plain;

    if (!view_picture(is, &vp)) {
        return 0.0f;
    }
    plain = view_default_zoom(is, vp);

    settled = view_zoom_settled(plain);
    want = settled * (direction > 0 ? VIEW_ZOOM_STEP : 1.0f / VIEW_ZOOM_STEP);
    want = av_clipf(want, FFMIN(VIEW_ZOOM_MIN, plain / VIEW_ZOOM_MAX),
                    VIEW_ZOOM_MAX);

    if ((settled - plain) * (want - plain) < 0.0f ||
        fabsf(want - plain) < plain * 0.005f) {
        view_zoom_want = 0.0f;
    } else {
        view_zoom_want = want;
        view_zoom_plain = plain;
    }

    return view_measure(is, vp, NULL, NULL);
}

float view_zoom_reset(VideoState *is) {
    Frame *vp;

    view_zoom_want = 0.0f;
    view_zoom_plain = 0.0f;
    view_center_s = 0.5f;
    view_center_t = 0.5f;
    if (!view_picture(is, &vp)) {
        return 0.0f;
    }

    return view_measure(is, vp, NULL, NULL);
}

void view_pan_by(VideoState *is, float dx, float dy) {
    SDL_Rect rect, clip;
    Frame *vp;
    float max_pan_x, max_pan_y, pan_x, pan_y;

    if (!view_picture(is, &vp)) {
        return;
    }
    view_measure(is, vp, &rect, &clip);

    view_pan_limits(rect.w, rect.h, clip.w, clip.h, &max_pan_x, &max_pan_y);
    pan_x = av_clipf(display_pan_x + dx, -max_pan_x, max_pan_x);
    pan_y = av_clipf(display_pan_y + dy, -max_pan_y, max_pan_y);

    view_center_from_display(0.5f - pan_x / (float)FFMAX(rect.w, 1),
                             0.5f - pan_y / (float)FFMAX(rect.h, 1));
}

static unsigned sub_rgba_generation;

static void prepare_subtitles(VideoState *is, Frame *vp) {
    Frame *sp;
    int plane_w, plane_h, max_dim;
    size_t need;

    is->render_params.sub_pixels = NULL;
    is->render_params.sub_width = 0;
    is->render_params.sub_height = 0;
    is->render_params.sub_stride = 0;

    if (!is->subtitle_st || frame_queue_nb_remaining(&is->subpq) <= 0) {
        return;
    }
    sp = frame_queue_peek(&is->subpq);
    if (sp->sub.format != 0 ||
        vp->pts < sp->pts + (sp->sub.start_display_time / 1000.0)) {
        return;
    }

    if (!sp->width || !sp->height) {
        sp->width = vp->width;
        sp->height = vp->height;
    }
    plane_w = sp->width;
    plane_h = sp->height;
    max_dim = display_max_texture_size();
    if (max_dim > 0 && (plane_w > max_dim || plane_h > max_dim)) {
        fit_within_max_dim(sp->width, sp->height, max_dim, &plane_w, &plane_h);
    }

    if (!sp->uploaded) {
        if (is->sub_rgba && (is->sub_rgba_w != plane_w || is->sub_rgba_h != plane_h)) {
            av_freep(&is->sub_rgba);
        }
        need = (size_t)plane_w * plane_h * 4;
        if (!is->sub_rgba) {
            is->sub_rgba = av_malloc(need);
            if (!is->sub_rgba) {
                return;
            }
            is->sub_rgba_w = plane_w;
            is->sub_rgba_h = plane_h;
        }
        memset(is->sub_rgba, 0, need);

        for (unsigned int i = 0; i < sp->sub.num_rects; i++) {
            AVSubtitleRect *sub_rect = sp->sub.rects[i];
            uint8_t *dst[4] = {NULL};
            int dst_pitch[4] = {0};
            int src_w, src_h;

            sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
            sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
            sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
            sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);
            src_w = sub_rect->w;
            src_h = sub_rect->h;
            if (src_w <= 0 || src_h <= 0) {
                continue;
            }

            if (plane_w != sp->width || plane_h != sp->height) {
                sub_rect->x = av_clip((int)((int64_t)sub_rect->x * plane_w / sp->width), 0, plane_w - 1);
                sub_rect->y = av_clip((int)((int64_t)sub_rect->y * plane_h / sp->height), 0, plane_h - 1);
                sub_rect->w = av_clip((int)((int64_t)src_w * plane_w / sp->width), 1, plane_w - sub_rect->x);
                sub_rect->h = av_clip((int)((int64_t)src_h * plane_h / sp->height), 1, plane_h - sub_rect->y);
            }

            is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                       src_w, src_h, AV_PIX_FMT_PAL8,
                                                       sub_rect->w, sub_rect->h, AV_PIX_FMT_RGBA,
                                                       0, NULL, NULL, NULL);
            if (!is->sub_convert_ctx) {
                return;
            }
            dst_pitch[0] = plane_w * 4;
            dst[0] = is->sub_rgba + (size_t)sub_rect->y * dst_pitch[0] +
                (size_t)sub_rect->x * 4;
            sws_scale(is->sub_convert_ctx, (const uint8_t *const *)sub_rect->data,
                      sub_rect->linesize, 0, src_h, dst, dst_pitch);
        }

        sp->width = plane_w;
        sp->height = plane_h;
        sp->uploaded = 1;
        sub_rgba_generation++;
    }

    if (!is->sub_rgba) {
        return;
    }
    is->render_params.sub_pixels = is->sub_rgba;
    is->render_params.sub_width = is->sub_rgba_w;
    is->render_params.sub_height = is->sub_rgba_h;
    is->render_params.sub_stride = is->sub_rgba_w * 4;
    is->render_params.sub_generation = sub_rgba_generation;
}

static void video_target_whole_window(VideoState *is) {
    SDL_Rect *rect = &is->render_params.target_rect;
    int bw = 0, bh = 0;

    SDL_GetWindowSizeInPixels(window, &bw, &bh);
    if (bw <= 0 || bh <= 0) {
        bw = is->width;
        bh = is->height;
    }
    *rect = (SDL_Rect){0, 0, bw, bh};
    is->render_params.target_clip = *rect;
    is->render_params.target_plain = *rect;
    is->render_storage_w = is->render_storage_h = 0;
}

static void video_update_target_rect(VideoState *is) {
    SDL_Rect *rect = &is->render_params.target_rect;

    if (is->video_st) {
        Frame *vp = frame_queue_peek_last(&is->pictq);
        int rotated = video_rotate == 90 || video_rotate == 270;

        calculate_display_rect(rect, &is->render_params.target_clip,
                               &is->render_params.target_plain, is->xleft,
                               is->ytop, is->width, is->height, vp->width,
                               vp->height, vp->sar);
        is->render_storage_w = rotated ? vp->height : vp->width;
        is->render_storage_h = rotated ? vp->width : vp->height;
        return;
    }

    video_target_whole_window(is);
}

void video_prepare_overlays(VideoState *is) {
    is->render_params.osd_pixels = NULL;
    is->render_params.sub_pixels = NULL;
    is->render_params.text_sub_pixels = NULL;
    is->render_params.next_frame = NULL;
    /* This thread owns the composited subtitle surface, so it frees it. */
    subtitles_reap();
    video_update_target_rect(is);
    osd_prepare(is);
    if (is->video_st && !subtitle_disable) {
        prepare_subtitles(is, frame_queue_peek_last(&is->pictq));
    }
}

static void video_image_display(VideoState *is) {
    Frame *vp = frame_queue_peek_last(&is->pictq);
    RenderMixFrame mix[LACHESIS_MAX_MIX_FRAMES];
    float mix_vsync = 0.0f;
    int ret;

    if (view360_enabled()) {
        renderer_update_360(renderer, sbs360_yaw, sbs360_pitch, sbs360_roll, sbs360_hfov);
    }
    is->render_params.still_image = is->is_still_image;
    is->render_params.rotate = video_rotate;
    is->render_params.frame_id = vp->id;
    is->last_render_serial = vp->serial;

    deinterlace_new_picture(is, vp);
    EqualizerValues eq = equalizer_get();
    is->render_params.eq_brightness = eq.brightness;
    is->render_params.eq_gamma = eq.gamma;
    is->render_params.eq_contrast = eq.contrast;
    is->render_params.eq_saturation = eq.saturation;
    deinterlace_prepare(is, vp);

    is->render_params.mix_num_frames =
        interpolate_frames(is, vp, mix, &mix_vsync);
    is->render_params.mix_frames = is->render_params.mix_num_frames ? mix : NULL;
    is->render_params.mix_vsync_duration = mix_vsync;

    ret = renderer_display(renderer, vp->frame, &is->render_params);

    is->render_params.mix_frames = NULL;
    is->render_params.mix_num_frames = 0;

    if (ret == AVERROR(EAGAIN)) {
        display_deferred++;
    } else if (ret == AVERROR(ERANGE)) {
        /* Doesn't imply the renderer doesn't work. */
    } else if (ret < 0) {
        int limit = render_ever_ok ? RENDER_FAULT_LIMIT_LATE : RENDER_FAULT_LIMIT;
        /* Can't be used to determine the renderer's health. */
        if (!(SDL_GetWindowFlags(window) &
              (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) &&
            !render_fault_event_sent && ++render_fail_streak >= limit) {
            SDL_Event event;
            SDL_zero(event);
            event.type = FF_RENDER_FAULT_EVENT;
            event.user.data1 = is;
            render_fault_event_sent = SDL_PushEvent(&event);
        }
    } else {
        render_ever_ok = 1;
        render_fail_streak = 0;
    }
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) {
        return;
    }
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        audio_device_close();
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        degrade_reset(is);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        subtitles_track_close();
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

#define READER_JOIN_TIMEOUT_US (2 * 1000 * 1000)

#define MAX_ABANDONED_STREAMS 8

static VideoState *abandoned_streams[MAX_ABANDONED_STREAMS];
static int num_abandoned_streams;
static int abandoned_untracked;

static int abandoned_threads_live(void) {
    if (abandoned_untracked) {
        return 1;
    }
    for (int i = 0; i < num_abandoned_streams; i++) {
        VideoState *is = abandoned_streams[i];

        if (is->read_tid && !SDL_GetAtomicInt(&is->read_thread_done)) {
            return 1;
        }
        if (is->audio_read_tid && !SDL_GetAtomicInt(&is->audio_read_thread_done)) {
            return 1;
        }
        if (is->sub_read_tid && !SDL_GetAtomicInt(&is->sub_read_thread_done)) {
            return 1;
        }
    }

    return 0;
}

#define PIPELINE_LOCK_TIMEOUT_US (2 * 1000 * 1000)
#define ABANDON_LOCK_TIMEOUT_US (250 * 1000)

static int pipeline_lock(VideoState *is, int64_t timeout_us) {
    int64_t deadline = av_gettime_relative() + timeout_us;

    while (!SDL_TryLockMutex(is->pipeline_mutex)) {
        if (av_gettime_relative() >= deadline) {
            return 0;
        }
        SDL_Delay(1);
    }

    return 1;
}

int pipeline_setup_begin(VideoState *is) {
    if (!pipeline_lock(is, PIPELINE_LOCK_TIMEOUT_US)) {
        return 0;
    }
    if (is->abort_request) {
        SDL_UnlockMutex(is->pipeline_mutex);
        return 0;
    }

    return 1;
}

void pipeline_setup_end(VideoState *is) {
    SDL_UnlockMutex(is->pipeline_mutex);
}

static int reader_join(SDL_Thread **tid, SDL_AtomicInt *done, int64_t deadline) {
    if (!*tid) {
        return 1;
    }
    while (!SDL_GetAtomicInt(done)) {
        if (av_gettime_relative() >= deadline) {
            return 0;
        }
        SDL_Delay(1);
    }
    SDL_WaitThread(*tid, NULL);
    *tid = NULL;

    return 1;
}

static void stream_abandon(VideoState *is) {
    is->abandoned = 1;

    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->audioq);
    packet_queue_abort(&is->subtitleq);
    SDL_SignalCondition(is->continue_read_thread);

    if (pipeline_lock(is, ABANDON_LOCK_TIMEOUT_US)) {
        renderer_release_frames(renderer);
        if (is->audio_stream >= 0) {
            decoder_abort(&is->auddec, &is->sampq);
            audio_device_close();
            decoder_destroy(&is->auddec);
        }
        if (is->video_stream >= 0) {
            decoder_abort(&is->viddec, &is->pictq);
            decoder_destroy(&is->viddec);
        }
        if (is->subtitle_stream >= 0 || is->sub_ic) {
            decoder_abort(&is->subdec, &is->subpq);
            decoder_destroy(&is->subdec);
            subtitles_track_close();
        }
        SDL_UnlockMutex(is->pipeline_mutex);
    } else {
        abandoned_untracked = 1;
    }

    if (is->read_tid) {
        SDL_DetachThread(is->read_tid);
    }
    if (is->audio_read_tid) {
        SDL_DetachThread(is->audio_read_tid);
    }
    if (is->sub_read_tid) {
        SDL_DetachThread(is->sub_read_tid);
    }
    if (num_abandoned_streams < MAX_ABANDONED_STREAMS) {
        abandoned_streams[num_abandoned_streams++] = is;
    } else {
        abandoned_untracked = 1;
    }
}

static void stream_detach(VideoState *is) {
    is->abort_request = 1;
    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->audioq);
    packet_queue_abort(&is->subtitleq);
    frame_queue_signal(&is->pictq);
    frame_queue_signal(&is->sampq);
    frame_queue_signal(&is->subpq);
    SDL_SignalCondition(is->continue_read_thread);

    if (pipeline_lock(is, ABANDON_LOCK_TIMEOUT_US)) {
        audio_device_close();
        SDL_UnlockMutex(is->pipeline_mutex);
    }
}

static int stream_close(VideoState *is) {
    int64_t deadline;
    int joined;

    is->abort_request = 1;
    SDL_SignalCondition(is->continue_read_thread);

    deadline = av_gettime_relative() + READER_JOIN_TIMEOUT_US;
    joined = reader_join(&is->read_tid, &is->read_thread_done, deadline);
    joined &= reader_join(&is->audio_read_tid, &is->audio_read_thread_done, deadline);
    joined &= reader_join(&is->sub_read_tid, &is->sub_read_thread_done, deadline);
    if (!joined) {
        stream_abandon(is);
        return 0;
    }

    renderer_release_frames(renderer);

    if (is->audio_stream >= 0) {
        if (is->audio_ic) {
            AVFormatContext *save = is->ic;
            is->ic = is->audio_ic;
            stream_component_close(is, is->audio_stream);
            is->ic = save;
        } else {
            stream_component_close(is, is->audio_stream);
        }
    }
    if (is->video_stream >= 0) {
        stream_component_close(is, is->video_stream);
    }
    if (is->subtitle_stream >= 0) {
        stream_component_close(is, is->subtitle_stream);
    } else if (is->sub_ic) {
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        subtitles_track_close();
        is->subtitle_st = NULL;
    }

    avformat_close_input(&is->ic);
    avformat_close_input(&is->audio_ic);
    avformat_close_input(&is->sub_ic);
    ytdl_chunked_free(&is->ytdl_vio);
    ytdl_chunked_free(&is->ytdl_aio);
    av_freep(&is->ytdl_source_url);
    av_freep(&is->ytdl_audio_url);
    archive_entry_close_avio(is->archive_avio);
    is->archive_avio = NULL;

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);
    deinterlace_close(is);
    SDL_DestroyCondition(is->continue_read_thread);
    SDL_DestroyMutex(is->pipeline_mutex);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    av_free(is->archive_path);
    av_free(is->entry_name);
    av_freep(&is->sub_rgba);
    av_free(is);

    return 1;
}

static void uninit_opts(void) {
    av_dict_free(&format_opts);
    for (int i = 0; i < nb_vfilters; i++) {
        av_freep(&vfilters_list[i]);
    }
    av_freep(&vfilters_list);
    nb_vfilters = 0;
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&hwaccel);
    av_freep(&hwaccel_codecs);
    av_freep(&afilters_opt);
    av_freep(&audio_spdif_opt);
    av_freep(&gpu_api_name);
    av_freep(&gpu_params);
    av_freep(&vulkan_swap_mode);
    av_freep(&shader_cache_dir);
    av_freep(&icc_profile);
    av_freep(&video_background);
    av_freep(&ytdl_path);
    av_freep(&ytdl_format);
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
        av_freep(&wanted_stream_spec[i]);
    }
    av_freep(&window_title);
    av_freep(&window_title_auto);
    av_freep(&window_title_path);
    av_freep(&window_title_shown);
    av_freep(&raise_token);
    av_freep(&input_filename);
    playlist_clear();
    for (int i = 0; i < n_input_args; i++) {
        av_freep(&input_args[i]);
    }
    av_freep(&input_args);
    n_input_args = 0;
}

static volatile sig_atomic_t quit_signal;
static volatile sig_atomic_t quit_signal_polled;

static int64_t shutdown_started;
static int64_t shutdown_marked;

static void shutdown_begin(void) {
    shutdown_started = shutdown_marked = av_gettime_relative();
}

static void shutdown_step(const char *what) {
    int64_t now = av_gettime_relative();

    log_verbose("Shutdown: %s took %.1f ms (%.1f ms in).\n", what,
                (now - shutdown_marked) / 1000.0,
                (now - shutdown_started) / 1000.0);
    shutdown_marked = now;
}

static int teardown_must_be_full(void) {
    return alloc_track_active();
}

static av_noreturn void exit_now(int status) {
    terminal_restore_now();
    shutdown_step("the exit");
    alloc_track_report();
    _Exit(status);
}

av_noreturn void do_exit(VideoState *is) {
    int status = quit_signal ? 123 : exit_status;
    int stranded_renderer = 0;
    int abandoned;

    quit_signal_polled = 0;
    shutdown_begin();

    if (is) {
        is->abort_request = 1;
        packet_queue_abort(&is->videoq);
        packet_queue_abort(&is->audioq);
        packet_queue_abort(&is->subtitleq);
        SDL_SignalCondition(is->continue_read_thread);
    }
    if (!teardown_must_be_full()) {
        audio_device_abandon();
    }

    single_shutdown();
    shutdown_step("the single instance listener");

    if (renderer) {
        renderer_quiesce(renderer, teardown_must_be_full());
        shutdown_step("quiescing the renderer");
        renderer_save_cache(renderer);
        shutdown_step("saving the shader cache");
    }
    if (is) {
        if (teardown_must_be_full()) {
            stream_close(is);
        } else {
            stream_detach(is);
        }
        shutdown_step("closing the stream");
    }
    abandoned = !screenshot_shutdown();
    shutdown_step("draining screenshots");
    abandoned |= abandoned_threads_live();

    if (abandoned || !teardown_must_be_full()) {
        exit_now(status);
    }

    if (renderer) {
        if (renderer_destroy(renderer)) {
            av_freep(&renderer);
        } else {
            stranded_renderer = 1;
        }
        shutdown_step("destroying the renderer");
    }
    if (window && !stranded_renderer) {
        SDL_DestroyWindow(window);
    }
    if (stranded_renderer) {
        exit_now(status);
    }
    uninit_opts();
    avformat_network_deinit();
    subtitles_uninit();
    osd_uninit();
    SDL_Quit();
    shutdown_step("the rest of the teardown");
    alloc_track_complete();
    exit(status);
}

static void sigterm_handler(int sig) {
    if (!quit_signal_polled || quit_signal) {
        terminal_restore_now();
        _Exit(123);
    }
    quit_signal = sig;
}

static float window_points_scale(void) {
    float density = 0.0f;

    if (window) {
        density = SDL_GetWindowPixelDensity(window);
    } else {
        const SDL_DisplayMode *mode =
            SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

        density = mode ? mode->pixel_density : 0.0f;
    }

    return density > 0.0f ? density : 1.0f;
}

static void window_size_for_content(int pic_width, int pic_height,
                                    AVRational sar, int rotate, int *out_w,
                                    int *out_h) {
    AVRational aspect_ratio = sar;
    int64_t width, height;
    int64_t max_width = INT64_MAX, max_height = INT64_MAX;
    float density = window_points_scale();
    SDL_Rect display_bounds;

    if (pic_width < 1) {
        pic_width = 1;
    }
    if (pic_height < 1) {
        pic_height = 1;
    }

    if (rotate == 90 || rotate == 270) {
        int tmp = pic_width;
        pic_width = pic_height;
        pic_height = tmp;
        if (aspect_ratio.num > 0 && aspect_ratio.den > 0) {
            aspect_ratio = av_make_q(aspect_ratio.den, aspect_ratio.num);
        }
    }

    if (aspect_ratio.num <= 0 || aspect_ratio.den <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = pic_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;

    if (SDL_GetDisplayBounds(window ? SDL_GetDisplayForWindow(window)
                                    : SDL_GetPrimaryDisplay(),
                             &display_bounds)) {
        max_width = (int64_t)(display_bounds.w * density * autofit_larger);
        max_height = (int64_t)(display_bounds.h * density * autofit_larger);
    }
    if (width > max_width || height > max_height) {
        height = max_height;
        width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
        if (width > max_width) {
            width = max_width;
            height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
        }
    }

    *out_w = FFMAX((int)lrintf((float)width / density), 1);
    *out_h = FFMAX((int)lrintf((float)height / density), 1);
}

static int sized_for_width;
static int sized_for_height;
static AVRational sized_for_sar;

static int window_rotate;

static int noted_rotate;

static int content_size_is_current(const Frame *vp) {
    return vp->width == sized_for_width && vp->height == sized_for_height &&
        vp->sar.num == sized_for_sar.num && vp->sar.den == sized_for_sar.den;
}

static void note_content_size(const Frame *vp) {
    sized_for_width = vp->width;
    sized_for_height = vp->height;
    sized_for_sar = vp->sar;
}

static int window_content_sized;

static void size_default_for_content(const Frame *vp) {
    window_content_sized = 1;
    note_content_size(vp);
    window_rotate = video_rotate;
    window_size_for_content(vp->width, vp->height, vp->sar, window_rotate,
                            &default_width, &default_height);
}

static void init_default_window_size(void) {
    window_size_for_content(1920, 1080, (AVRational){1, 1}, 0, &default_width,
                            &default_height);
}

int note_window_pixel_size(int w, int h) {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    screen_width = w;
    screen_height = h;

    return 1;
}

void update_screen_size(void) {
    int w = 0, h = 0;

    if (!window) {
        return;
    }
    SDL_GetWindowSizeInPixels(window, &w, &h);
    note_window_pixel_size(w, h);
}

int video_adopt_window_size(VideoState *is) {
    if (screen_width <= 0 || screen_height <= 0) {
        return 0;
    }
    is->width = screen_width;
    is->height = screen_height;

    return 1;
}

float window_pixel_density(void) {
    float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;

    return density > 0.0f ? density : 1.0f;
}

static const char *file_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
}

static char *make_default_window_title(const char *path,
                                       const char *archive_path,
                                       const char *entry_name) {
    const char *display;
    char *owned = NULL;

    if (archive_path && entry_name) {
        owned = av_asprintf("%s | %s", file_basename(archive_path), entry_name);
        display = owned ? owned : entry_name;
    } else {
        if (!path) {
            path = input_filename;
        }
        if (!path) {
            return NULL;
        }
        display = strstr(path, "://") ? path : file_basename(path);
    }

    char *title;
    if (playlist_size > 1) {
        title = av_asprintf("%s - %s [%d/%d]", program_name, display,
                            playlist_pos + 1, playlist_size);
    } else {
        title = av_asprintf("%s - %s", program_name, display);
    }
    av_free(owned);

    return title;
}

static const char *current_window_title(VideoState *is) {
    if (window_title) {
        return window_title;
    }
    if (SDL_GetAtomicInt(&is->open_phase) == STREAM_OPEN_DONE && window_title_auto) {
        return window_title_auto;
    }

    return window_title_path;
}

static void refresh_window_title(VideoState *is) {
    const char *title = current_window_title(is);

    if (!window || !title ||
        (window_title_shown && !strcmp(window_title_shown, title))) {
        return;
    }
    av_free(window_title_shown);
    window_title_shown = av_strdup(title);
    SDL_SetWindowTitle(window, title);
}

#define WINDOW_SIZE_SLACK 4

static int window_is_ours_to_size(void) {
    return !(SDL_GetWindowFlags(window) &
             (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED |
              SDL_WINDOW_MINIMIZED));
}

static int window_want_w;
static int window_want_h;
static int64_t window_want_us;
static int window_want_tries;

static void forget_window_size(void) {
    window_want_w = 0;
    window_want_h = 0;
    window_want_us = 0;
    window_want_tries = 0;
}

static void want_window_size(int w, int h) {
    window_want_w = w;
    window_want_h = h;
    window_want_us = av_gettime_relative();
    window_want_tries = 0;
}

static int window_size_still_wanted(void) {
    return window_want_w > 0 && window_want_h > 0 &&
        av_gettime_relative() - window_want_us <= WINDOW_INSIST_US;
}

static void insist_window_size(void) {
    if (window_size_still_wanted() && !is_fullscreen &&
        window_is_ours_to_size()) {
        SDL_SetWindowSize(window, window_want_w, window_want_h);
    }
}

static int hold_output(void) {
    return renderer_pause_output(renderer);
}

static void drop_output(int held) {
    if (held) {
        renderer_resume_output(renderer);
    }
}

void present_pacing_reset(void) {
    renderer_drop_present_feedback(renderer);
    present_reset();
}

static int display_info_stale;

static void refresh_display_info(VideoState *is) {
    int ret;

    if (!display_info_stale || !renderer) {
        return;
    }
    ret = renderer_refresh_display_info(renderer, window);
    if (ret < 0) {
        return;
    }
    display_info_stale = 0;
    if (ret > 0) {
        is->force_refresh = 1;
    }
}

void note_display_info_change(VideoState *is) {
    display_info_stale = 1;
    refresh_display_info(is);
}

static void apply_window_geometry(int w, int h) {
    want_window_size(w, h);
    SDL_SetWindowSize(window, w, h);
    if (!SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED)) {
        SDL_ClearError();
    }
}

void window_want_raise(const char *token) {
    av_freep(&raise_token);
    if (token && *token) {
        raise_token = av_strdup(token);
    }
    raise_started_us = av_gettime_relative();
    raise_asked = 0;
    raise_wanted = 1;
}

static int raise_arm(void) {
    if (!raise_wanted) {
        return 0;
    }
    if (raise_token) {
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(),
                                   "XDG_ACTIVATION_TOKEN", raise_token, true);
    }

    return 1;
}

static void raise_spent(void) {
    raise_wanted = 0;
    raise_asked = 0;
    av_freep(&raise_token);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "XDG_ACTIVATION_TOKEN");
}

static void finish_raise(void) {
    if (!raise_wanted || !window) {
        return;
    }
    if (!raise_asked) {
        raise_asked = 1;
        raise_arm();
        SDL_RaiseWindow(window);
    }
    if (!raise_token || (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
        raise_spent();
        return;
    }
    if (av_gettime_relative() - raise_started_us < RAISE_GRACE_US) {
        return;
    }

    raise_spent();
}

static void video_follow_content_size(VideoState *is) {
    int w, h;
    Frame *vp;

    if (!is->video_st) {
        return;
    }

    vp = frame_queue_peek_last(&is->pictq);
    if (video_rotate == noted_rotate && content_size_is_current(vp)) {
        return;
    }
    noted_rotate = window_rotate = video_rotate;
    note_content_size(vp);

    if (window_content_sized && !window_resize) {
        return;
    }
    window_content_sized = 1;

    window_size_for_content(vp->width, vp->height, vp->sar, window_rotate, &w,
                            &h);

    if (w != default_width || h != default_height) {
        default_width = w;
        default_height = h;
        if (!is_fullscreen) {
            int held = hold_output();

            apply_window_geometry(default_width, default_height);
            SDL_SyncWindow(window);
            update_screen_size();
            video_adopt_window_size(is);
            drop_output(held);
        }
    }
}

void note_window_resized(VideoState *is, int w, int h) {
    if (!window || !window_placed || !window_want_w) {
        return;
    }
    if (is_fullscreen || !window_is_ours_to_size() ||
        !window_size_still_wanted()) {
        forget_window_size();
        return;
    }
    if (abs(w - window_want_w) <= WINDOW_SIZE_SLACK &&
        abs(h - window_want_h) <= WINDOW_SIZE_SLACK) {
        return;
    }
    if (window_want_tries >= WINDOW_INSIST_TRIES) {
        forget_window_size();
        return;
    }
    window_want_tries++;
    insist_window_size();
    if (is) {
        update_screen_size();
        video_adopt_window_size(is);
    }
}

static void place_window(void) {
    int held;

    if (window_placed) {
        return;
    }
    held = hold_output();
    window_placed = 1;
    window_rotate = noted_rotate = video_rotate;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        apply_window_geometry(default_width, default_height);
    }
    SDL_ShowWindow(window);
    drop_output(held);
}

static void window_open_bare(VideoState *is) {
    int held;

    place_window();
    held = hold_output();
    SDL_SyncWindow(window);
    present_update_display_mode();

    update_screen_size();
    if (!video_adopt_window_size(is)) {
        float scale = window_points_scale();

        is->width = FFMAX((int)lrintf(default_width * scale), 1);
        is->height = FFMAX((int)lrintf(default_height * scale), 1);
    }
    drop_output(held);
}

static int video_open(VideoState *is) {
    if (!window_placed) {
        if (is->video_st) {
            size_default_for_content(frame_queue_peek_last(&is->pictq));
        }
    } else {
        video_follow_content_size(is);
    }
    window_open_bare(is);

    return 0;
}

static int window_occluded(void) {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_OCCLUDED);
}

static int video_display_opening(VideoState *is) {
    if (!renderer) {
        return 0;
    }
    if (!is->window_opened) {
        is->window_opened = 1;
        window_open_bare(is);
    }
    if (window_occluded()) {
        return 0;
    }

    is->render_params.osd_pixels = NULL;
    is->render_params.sub_pixels = NULL;
    is->render_params.text_sub_pixels = NULL;
    is->render_params.next_frame = NULL;
    is->render_params.present_done_us = 0;
    is->render_params.present_block_us = 0;
    is->render_params.present_source = PRESENT_SOURCE_SWAP;
    is->render_params.present_display_us = 0;
    is->render_params.present_refresh_us = 0;
    is->render_params.eq_brightness = 0;
    is->render_params.eq_gamma = 0;
    is->render_params.eq_contrast = 0;
    is->render_params.eq_saturation = 0;
    video_target_whole_window(is);
    if (renderer_display_blank(renderer, &is->render_params) == AVERROR(EAGAIN)) {
        display_deferred++;
        return 0;
    }

    return 1;
}

static void apply_present_feedback(void) {
    RendererPresentFeedback fb;

    while (renderer_take_present_feedback(renderer, &fb)) {
        if (fb.source != PRESENT_SOURCE_SWAP) {
            present_note_present(fb.done_us);
            if (fb.display_us > 0) {
                present_feedback_display(fb.source, fb.display_us,
                                         fb.refresh_us);
            }
        } else if (fb.done_us > 0) {
            present_feedback(fb.done_us - fb.block_us, fb.done_us);
        }
    }
}

/* Returns whether the window could be painted at all. */
static int video_display(VideoState *is) {
    int owed = display_deferred;

    if (!renderer) {
        return 0;
    }

    if (!is->window_opened || !window_placed) {
        is->window_opened = 1;
        video_open(is);
    } else {
        video_follow_content_size(is);
    }

    if (window_occluded()) {
        return 0;
    }

    is->render_params.present_done_us = 0;
    is->render_params.present_block_us = 0;
    is->render_params.present_source = PRESENT_SOURCE_SWAP;
    is->render_params.present_display_us = 0;
    is->render_params.present_refresh_us = 0;
    video_prepare_overlays(is);

    if (is->video_st) {
        video_image_display(is);
    } else {
        /* Nothing to do. */
        is->render_params.eq_brightness = 0;
        is->render_params.eq_gamma = 0;
        is->render_params.eq_contrast = 0;
        is->render_params.eq_saturation = 0;
        deinterlace_clear(is);
        if (renderer_display_blank(renderer, &is->render_params) ==
            AVERROR(EAGAIN)) {
            display_deferred++;
        }
    }

    if (is->audio_start_pending) {
        is->audio_start_pending = 0;
        audio_device_resume();
    }

    return display_deferred == owed;
}

static double clock_rate(const Clock *c) {
    return c->speed * playback_speed;
}

double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial) {
        return LACHESIS_NAN;
    }
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - clock_rate(c));
    }
}

void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void set_clock(Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed) {
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, LACHESIS_NAN, -1);
}

void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD)) {
        set_clock(c, slave_clock, slave->serial);
    }
}

int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st) {
            return AV_SYNC_VIDEO_MASTER;
        } else {
            return AV_SYNC_AUDIO_MASTER;
        }
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st) {
            return AV_SYNC_AUDIO_MASTER;
        } else {
            return AV_SYNC_EXTERNAL_CLOCK;
        }
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
    case AV_SYNC_VIDEO_MASTER:
        val = get_clock(&is->vidclk);
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = get_clock(&is->audclk);
        break;
    default:
        val = get_clock(&is->extclk);
        break;
    }

    return val;
}

int video_stream_advances(VideoState *is) {
    return is->video_st &&
        !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC);
}

static int duration_counts_from_zero(const AVFormatContext *ic) {
    static const char *const formats[] = {"matroska,webm", "asf", "asf_o"};

    if (!ic->iformat || !ic->iformat->name ||
        ic->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE) {
        return 0;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(formats); i++) {
        if (!strcmp(ic->iformat->name, formats[i])) {
            return 1;
        }
    }

    return 0;
}

double playhead_origin(const VideoState *is) {
    if (is->ic && is->ic->start_time != AV_NOPTS_VALUE) {
        return is->ic->start_time / (double)AV_TIME_BASE;
    }

    return 0.0;
}

double playhead_length(const VideoState *is) {
    double length = 0.0;

    if (is->ic && is->ic->duration != AV_NOPTS_VALUE && is->ic->duration > 0) {
        double origin = playhead_origin(is);

        length = is->ic->duration / (double)AV_TIME_BASE;
        if (origin > 0.0 && length > origin &&
            duration_counts_from_zero(is->ic)) {
            length -= origin;
        }
    }
    if (length > 0.0 && is->observed_length > length) {
        length = is->observed_length;
    }

    return length;
}

double playhead_elapsed(const VideoState *is, double pos) {
    double length = playhead_length(is);

    if (isnan(pos)) {
        return LACHESIS_NAN;
    }
    pos -= playhead_origin(is);
    if (pos < 0.0) {
        pos = 0.0;
    }
    if (length > 0.0 && pos > length) {
        pos = length;
    }

    return pos;
}

double playhead_clamp(const VideoState *is, double pos) {
    double origin = playhead_origin(is);
    double length = playhead_length(is);

    if (isnan(pos) || pos < origin) {
        return origin;
    }
    if (length > 0.0 && pos > origin + length) {
        pos = origin + length;
    }

    return pos;
}

static int decoder_exact_pending(Decoder *d, int armed_serial) {
    if (armed_serial < 0 || d->exact_done_serial == armed_serial ||
        d->finished == armed_serial) {
        return 0;
    }

    return d->queue->serial == armed_serial;
}

static int exact_seek_pending(VideoState *is) {
    int armed = 0;

    if (isnan(is->exact_seek_pts)) {
        return 0;
    }
    if (is->exact_seek_video_serial >= 0) {
        if (!decoder_exact_pending(&is->viddec, is->exact_seek_video_serial)) {
            return 0;
        }
        armed = 1;
    }
    if (is->exact_seek_audio_serial >= 0) {
        if (!decoder_exact_pending(&is->auddec, is->exact_seek_audio_serial)) {
            return 0;
        }
        armed = 1;
    }

    return armed;
}

double effective_playhead(VideoState *is) {
    double pos = get_master_clock(is);

    if (isnan(pos) || get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK) {
        double decoded = is->audio_st ? get_clock(&is->audclk) : LACHESIS_NAN;

        if (isnan(decoded) && video_stream_advances(is)) {
            decoded = get_clock(&is->vidclk);
        }
        if (!isnan(decoded)) {
            pos = decoded;
        }
    }
    if (isnan(pos)) {
        pos = get_clock(&is->extclk);
    }

    if (is->seek_flags & AVSEEK_FLAG_BYTE) {
        return isnan(pos) ? is->start_playhead : pos;
    }

    if (is->seek_req) {
        return is->seek_pos / (double)AV_TIME_BASE;
    }
    if (exact_seek_pending(is)) {
        return is->exact_seek_pts;
    }
    if (isnan(pos)) {
        pos = is->start_playhead;
    }

    return pos;
}

static void check_external_clock_speed(VideoState *is) {
    if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0) {
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

void exact_seek_cancel(VideoState *is) {
    is->exact_seek_pts = LACHESIS_NAN;
    is->exact_seek_video_serial = -1;
    is->exact_seek_audio_serial = -1;
}

static double stream_start_seconds(const AVStream *st) {
    if (!st || st->start_time == AV_NOPTS_VALUE) {
        return LACHESIS_NAN;
    }

    return st->start_time * av_q2d(st->time_base);
}

double aligned_start_pts(VideoState *is) {
    double video_start, audio_start, lead;

    if (is->audio_ic || !is->audio_st || !video_stream_advances(is)) {
        return LACHESIS_NAN;
    }
    video_start = stream_start_seconds(is->video_st);
    audio_start = stream_start_seconds(is->audio_st);
    if (isnan(video_start) || isnan(audio_start)) {
        return LACHESIS_NAN;
    }

    lead = video_start - audio_start;
    if (lead <= AV_SYNC_THRESHOLD_MAX || lead >= AV_NOSYNC_THRESHOLD) {
        return LACHESIS_NAN;
    }

    return video_start;
}

void exact_seek_arm(VideoState *is, int64_t target) {
    double length;

    exact_seek_cancel(is);
    if (target == AV_NOPTS_VALUE || is->is_still_image) {
        return;
    }
    length = playhead_length(is);
    if (length > 0.0 &&
        target / (double)AV_TIME_BASE >= playhead_origin(is) + length) {
        return;
    }
    is->exact_seek_pts = target / (double)AV_TIME_BASE;
    if (is->video_st && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        is->exact_seek_video_serial = is->videoq.serial;
    }
    if (is->audio_st && !audio_spdif_active()) {
        is->exact_seek_audio_serial = is->audioq.serial;
    }
}

static int exact_seek_drop(VideoState *is, Decoder *d, int armed_serial,
                           double pts, double duration) {
    if (armed_serial < 0 || armed_serial != d->pkt_serial ||
        d->exact_done_serial == d->pkt_serial) {
        return 0;
    }
    if (isnan(pts) || isnan(is->exact_seek_pts) ||
        pts + duration >= is->exact_seek_pts - EXACT_SEEK_SLACK ||
        pts < is->exact_seek_pts - EXACT_SEEK_MAX_RUNUP) {
        d->exact_done_serial = d->pkt_serial;
        return 0;
    }

    return 1;
}

static void stream_seek_exact_from(VideoState *is, int64_t pos, int64_t exact_pts);

static void exact_seek_overshot(VideoState *is, double pts) {
    double target = is->exact_seek_pts;
    double backoff, from;

    if (isnan(target) || isnan(pts) || pts <= target + EXACT_SEEK_SLACK ||
        is->viddec.exact_dropped_serial == is->viddec.pkt_serial) {
        return;
    }
    if (is->exact_seek_backoff_target != target) {
        is->exact_seek_backoff_target = target;
        is->exact_seek_backoff = 0.0;
    }
    if (is->exact_seek_backoff >= EXACT_SEEK_MAX_RUNUP) {
        return;
    }
    backoff = is->exact_seek_backoff
        ? FFMIN(is->exact_seek_backoff * 4.0, EXACT_SEEK_MAX_RUNUP)
        : EXACT_SEEK_BACKOFF;
    from = playhead_clamp(is, target - backoff);
    if (from >= target) {
        return;
    }
    is->exact_seek_backoff = backoff;
    stream_seek_exact_from(is, (int64_t)(from * AV_TIME_BASE),
                           (int64_t)(target * AV_TIME_BASE));
}

int exact_seek_drop_video(VideoState *is, double pts) {
    Decoder *dec = &is->viddec;
    int settled = dec->exact_done_serial == dec->pkt_serial;

    if (exact_seek_drop(is, dec, is->exact_seek_video_serial, pts, 0)) {
        dec->exact_dropped_serial = dec->pkt_serial;
        return 1;
    }
    if (!settled && dec->exact_done_serial == dec->pkt_serial &&
        is->exact_seek_video_serial == dec->pkt_serial) {
        exact_seek_overshot(is, pts);
    }

    return 0;
}

int exact_seek_drop_audio(VideoState *is, double pts, double duration) {
    return exact_seek_drop(is, &is->auddec, is->exact_seek_audio_serial, pts, duration);
}

/* Replace whatever is pending rather than dropping the request. */
static void stream_seek_to(VideoState *is, int64_t pos, int64_t rel, int by_bytes,
                           int exact, int64_t exact_pts) {
    is->seek_pos = pos;
    is->seek_rel = rel;
    is->seek_exact = exact;
    is->seek_exact_pts = exact_pts;
    is->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes) {
        is->seek_flags |= AVSEEK_FLAG_BYTE;
    }
    is->seek_serial++;
    is->seek_req = 1;
    SDL_SignalCondition(is->continue_read_thread);
}

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes) {
    stream_seek_to(is, pos, rel, by_bytes, 0, pos);
}

void stream_seek_exact(VideoState *is, int64_t pos) {
    stream_seek_to(is, pos, 0, 0, 1, pos);
}

static void stream_seek_exact_from(VideoState *is, int64_t pos, int64_t exact_pts) {
    stream_seek_to(is, pos, 0, 0, 1, exact_pts);
}

static void stream_toggle_pause(VideoState *is) {
    is->start_pause_pending = 0;
    if (is->paused) {
        double now = av_gettime_relative() / 1000000.0;

        is->frame_timer += now - is->vidclk.last_updated;
        if (is->frame_timer > now) {
            is->frame_timer = now;
        }
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
        is->audclk_drift_valid = 0;
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void toggle_pause(VideoState *is) {
    stream_toggle_pause(is);
    is->step = 0;
    is->step_from_play = 0;
    osd_show_status();
}

static void ab_loop_fmt_time(double t, char *buf, size_t size) {
    if (isnan(t) || t < 0) {
        t = 0;
    }
    int total = (int)(t + 0.5);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    if (h > 0) {
        snprintf(buf, size, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, size, "%d:%02d", m, s);
    }
}

static void ab_loop_reset(void) {
    ab_loop_a = LACHESIS_NAN;
    ab_loop_b = LACHESIS_NAN;
}

void ab_loop_toggle(VideoState *is) {
    char a_buf[32], b_buf[32];
    osd_show_position();
    double pos = effective_playhead(is);

    if (isnan(pos) || pos < playhead_origin(is)) {
        pos = playhead_origin(is);
    }

    if (isnan(ab_loop_a)) {
        ab_loop_a = pos;
    } else if (isnan(ab_loop_b)) {
        if (pos <= ab_loop_a) {
            ab_loop_b = ab_loop_a;
            ab_loop_a = pos;
        } else {
            ab_loop_b = pos;
        }
        ab_loop_fmt_time(playhead_elapsed(is, ab_loop_a), a_buf, sizeof(a_buf));
        ab_loop_fmt_time(playhead_elapsed(is, ab_loop_b), b_buf, sizeof(b_buf));
        osd_show_message("A-B loop: %s - %s", a_buf, b_buf);
        /* Snap back to A. */
        stream_seek(is, (int64_t)(ab_loop_a * AV_TIME_BASE),
                    (int64_t)((ab_loop_a - pos) * AV_TIME_BASE), 0);
    } else {
        ab_loop_reset();
        osd_show_message("A-B loop: cleared");
    }
}

static void ab_loop_check(VideoState *is) {
    if (isnan(ab_loop_a) || isnan(ab_loop_b) || is->paused || is->seek_req) {
        return;
    }
    double pos = get_master_clock(is);
    if (isnan(pos) || pos < ab_loop_b) {
        return;
    }
    stream_seek(is, (int64_t)(ab_loop_a * AV_TIME_BASE),
                (int64_t)((ab_loop_a - pos) * AV_TIME_BASE), 0);
}

void reanchor_clocks(VideoState *is) {
    if (!is) {
        return;
    }
    set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
}

void set_playback_speed(VideoState *is, double speed) {
    if (audio_spdif_active()) {
        return;
    }
    speed = round(speed / PLAYBACK_SPEED_STEP) * PLAYBACK_SPEED_STEP;
    speed = FFMAX(PLAYBACK_SPEED_MIN, FFMIN(PLAYBACK_SPEED_MAX, speed));
    osd_show_position();
    if (speed == playback_speed) {
        osd_show_message("Speed: %d%%", (int)lrint(playback_speed * 100.0));
        return;
    }
    reanchor_clocks(is);
    playback_speed = speed;
    audio_speed_serial++;
    osd_show_message("Speed: %d%%", (int)lrint(playback_speed * 100.0));
}

static void reset_playback_speed(void) {
    playback_speed = 1.0;
    audio_speed_serial++;
}

void step_to_next_frame(VideoState *is) {
    if (!video_stream_advances(is)) {
        return;
    }
    is->step = 1;
}

static int step_needs_seek(VideoState *is, double *from, double *to) {
    Frame *lastvp, *vp;
    double wait;

    if (!is->pictq.rindex_shown || frame_queue_nb_remaining(&is->pictq) <= 0 ||
        SDL_GetAtomicInt(&is->seek_by_bytes) > 0 || is->seek_req) {
        return 0;
    }

    lastvp = frame_queue_peek_last(&is->pictq);
    vp = frame_queue_peek(&is->pictq);
    if (vp->serial != is->videoq.serial || vp->serial != lastvp->serial ||
        isnan(lastvp->pts) || isnan(vp->pts)) {
        return 0;
    }

    wait = vp->pts - lastvp->pts;
    if (wait <= 0.0 || wait > is->max_frame_duration) {
        return 0;
    }

    if (wait / playback_speed <= AV_NOSYNC_THRESHOLD) {
        return 0;
    }

    *from = lastvp->pts;
    *to = vp->pts;

    return 1;
}

void frame_step(VideoState *is) {
    double from, to;

    if (!video_stream_advances(is)) {
        osd_show_message("No frames to step");
        return;
    }

    is->step_key_held = 1;

    if (step_needs_seek(is, &from, &to)) {
        if (!is->paused) {
            is->step_from_play = 0;
        }
        stream_seek(is, (int64_t)(to * AV_TIME_BASE),
                    (int64_t)((to - from) * AV_TIME_BASE), 0);
    } else {
        if (!is->paused) {
            is->step_from_play = 1;
        }
        step_to_next_frame(is);
    }

#if 0
    osd_show_seek();
#endif
}

static double compute_target_delay(double delay, VideoState *is) {
    double diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        is->last_av_diff = isnan(diff) ? LACHESIS_NAN : -diff;
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (fabs(diff) <= AV_SYNC_THRESHOLD_MAX) {
                double base = delay > 0 ? delay : AV_SYNC_THRESHOLD_MIN;
                double change = av_clipd(diff * AV_SYNC_SLEW_GAIN,
                                         -base * AV_SYNC_SLEW_FACTOR,
                                         base * AV_SYNC_SLEW_FACTOR);
                delay = FFMAX(0, delay + change);
            } else if (diff < 0) {
                delay = FFMAX(0, delay + diff);
            } else {
                delay = delay + FFMIN(diff, AV_SYNC_MAX_HOLD * playback_speed);
            }
        }
    } else {
        is->last_av_diff = LACHESIS_NAN;
    }

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration) {
            return vp->duration;
        } else {
            return duration;
        }
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int serial) {
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

static int open_is_slow(VideoState *is) {
    return SDL_GetAtomicInt(&is->open_phase) != STREAM_OPEN_DONE &&
        av_gettime_relative() - is->open_started_us >= SLOW_OPEN_US;
}

/* The OSD runs on wall clock time so it has to be painted even when the video is not. */
static int osd_wants_repaint(VideoState *is, double now) {
    unsigned state;

    if (display_disable || window_occluded()) {
        return 0;
    }

    state = osd_state(is);
    if (state != is->osd_state) {
        return 1;
    }
    if (!state || is->paused) {
        return 0;
    }

    return now - is->last_draw_time >= OSD_ONLY_REFRESH_RATE;
}

static void osd_keep_alive(VideoState *is, double now) {
    if (osd_wants_repaint(is, now)) {
        is->force_refresh = 1;
    }
}

static void video_refresh(void *opaque, double *remaining_time) {
    VideoState *is = opaque;
    double time;
    int interp_painted = 0;
    int painted = 0;

    Frame *sp, *sp2;

    display_deferred = 0;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) {
        check_external_clock_speed(is);
    }

    if (!display_disable && !SDL_GetAtomicInt(&is->streams_selected) &&
        open_is_slow(is)) {
        double now = av_gettime_relative() / 1000000.0;

        if ((is->force_refresh || !is->window_opened) &&
            video_display_opening(is)) {
            is->last_draw_time = now;
        }
    } else if (!display_disable && SDL_GetAtomicInt(&is->streams_selected) &&
               !is->video_st) {
        double now = av_gettime_relative() / 1000000.0;

        if ((is->force_refresh || !is->window_opened ||
             osd_wants_repaint(is, now)) &&
            video_display(is)) {
            is->last_draw_time = now;
        }
    }

    if (is->video_st) {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            if (is->videoq.nb_packets > 0) {
                *remaining_time = FFMIN(*remaining_time, 0.002);
            }
            if (is->pictq.rindex_shown) {
                Frame *lastvp = frame_queue_peek_last(&is->pictq);

                deinterlace_pace(is, lastvp->duration / playback_speed,
                                 av_gettime_relative() / 1000000.0,
                                 remaining_time);
            }
            osd_keep_alive(is, av_gettime_relative() / 1000000.0);
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial ||
                degrade_stale_frame(is, vp->pts, vp->serial)) {
                deinterlace_retire_frame(is);
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial) {
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            if (is->paused && !is->step) {
                osd_keep_alive(is, av_gettime_relative() / 1000000.0);
                goto display;
            }

            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is) / playback_speed;

            time = av_gettime_relative() / 1000000.0;
            interp_painted = interpolate_pace(is, time, remaining_time);
            painted |= interp_painted;
            deinterlace_pace(is, delay, time, remaining_time);

            if (!benchmark) {
                double ideal = is->frame_timer + delay;
                double target = present_snap(ideal, time);
                double lead = 0;
                if (target != ideal) {
                    lead = FFMIN(PRESENT_LEAD_MAX, present_vsync_sec() * 0.25);
                }
                if (time < target - lead) {
                    *remaining_time = FFMIN(target - lead - time, *remaining_time);
                    if (target - lead - time >= OSD_ONLY_REFRESH_RATE) {
                        osd_keep_alive(is, time);
                    }
                    goto display;
                }
            }

            is->frame_timer += delay;
            if (!benchmark && delay > 0 && time - is->frame_timer > AV_SYNC_RESYNC_THRESHOLD) {
                is->frame_timer = time;
            }

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts)) {
                update_video_pts(is, vp->pts, vp->serial);
            }
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp) / playback_speed;
                int64_t last_done = present_last_done_us();
                int presenting = display_disable ||
                    (last_done > 0 &&
                     av_gettime_relative() - last_done < 100000);
                /* clang-format off */
                if (!benchmark && !is->step && presenting &&
                    (playback_speed > 1.0 ||
                     get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) &&
                    time > is->frame_timer + duration) {
                    /* clang-format on */
                    is->frame_drops_late++;
                    deinterlace_retire_frame(is);
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1) {
                        sp2 = frame_queue_peek_next(&is->subpq);
                    } else {
                        sp2 = NULL;
                    }

                    /* clang-format off */
                    if (sp->serial != is->subtitleq.serial ||
                        (is->vidclk.pts >
                         (sp->pts + (sp->sub.end_display_time / 1000.0))) ||
                        (sp2 &&
                         is->vidclk.pts >
                             (sp2->pts +
                              (sp2->sub.start_display_time / 1000.0)))) {
                        /* clang-format on */
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            deinterlace_retire_frame(is);
            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step) {
                if (is->paused) {
                    is->step = 0;
                } else if (is->step_from_play && !is->step_key_held) {
                    is->step = 0;
                    is->step_from_play = 0;
                } else {
                    is->step = 0;
                    stream_toggle_pause(is);
                }
            }
        }
    display:
        if (!display_disable && is->force_refresh && !interp_painted &&
            is->pictq.rindex_shown) {
            painted |= video_display(is);
        }
        if (painted) {
            is->last_draw_time = av_gettime_relative() / 1000000.0;
        }
    }
    is->force_refresh = display_deferred != 0;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    static uint64_t next_frame_id = 1;
    int64_t stall_t0 = av_gettime_relative();
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq))) {
        return -1;
    }
    degrade_note_stall(is, av_gettime_relative() - stall_t0);

    vp->sar = src_frame->sample_aspect_ratio;

    frame_visible_size(src_frame, &vp->width, &vp->height);
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;
    vp->id = next_frame_id++;

    if (serial != is->pictq_last_serial) {
        if (!isnan(pts)) {
            is->audio_catchup_startup = is->pictq_last_serial == -1;
            is->audio_catchup_pts = pts;
            is->audio_catchup_serial = serial;
        }
        is->pictq_last_serial = serial;
    }

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);

    return 0;
}

static void hwframe_download_inplace(AVFrame *frame) {
    static int warned = 0;
    static int announced = 0;
    AVFrame *sw = av_frame_alloc();
    int ret;

    if (!sw) {
        return;
    }

    ret = av_hwframe_transfer_data(sw, frame, 0);
    if (ret < 0) {
        if (!warned) {
            warned = 1;
            log_warn("Failed to download hardware frame to system memory: %s.\n", av_err2str(ret));
        }
        av_frame_free(&sw);
        return;
    }

    if (!announced) {
        announced = 1;
        log_info("Copying decoded frames from the GPU to system memory.\n");
    }

    av_frame_copy_props(sw, frame);
    if (sw->crop_left || sw->crop_top || sw->crop_right || sw->crop_bottom) {
        static int crop_warned = 0;
        int crop_ret = av_frame_apply_cropping(sw, 0);

        if (crop_ret < 0 && !crop_warned) {
            crop_warned = 1;
            log_warn("Failed to apply frame cropping: %s.\n", av_err2str(crop_ret));
        }
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, sw);
    av_frame_free(&sw);
}

static int get_video_frame(VideoState *is, AVFrame *frame) {
    int got_picture;
    int had_packets = is->videoq.nb_packets > 0;
    int64_t decode_t0 = av_gettime_relative();

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0) {
        return -1;
    }

    if (got_picture) {
        double dpts = LACHESIS_NAN;
        int64_t decode_us = av_gettime_relative() - decode_t0 - is->viddec.wait_us;
        if (decode_us < 0) {
            decode_us = 0;
        }

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        if (exact_seek_drop_video(is, dpts)) {
            av_frame_unref(frame);
            return 0;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
        int64_t interval_us =
            (fr.num > 0 && fr.den > 0) ? (int64_t)(1000000.0 * fr.den / fr.num) : 0;
        int64_t span_us = interval_us;
        if (!isnan(dpts)) {
            if (is->viddec.pkt_serial == is->decode_span_serial &&
                !isnan(is->decode_span_pts)) {
                double span = dpts - is->decode_span_pts;
                if (span > 0.0 && span < 1.0) {
                    span_us = (int64_t)(span * 1000000.0);
                }
            }
            is->decode_span_pts = dpts;
            is->decode_span_serial = is->viddec.pkt_serial;
        }
        int64_t budget_us = playback_speed > 0.0
            ? (int64_t)(span_us / playback_speed)
            : span_us;

        degrade_frame(is, dpts, decode_us, budget_us, had_packets);

        if (degrade_drop_late_frame(is, dpts, interval_us)) {
            av_frame_unref(frame);
            got_picture = 0;
        }
    }

    return got_picture;
}

int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                          AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret;
    unsigned int i;
    unsigned int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name = av_strdup("in");
        alloc_track_disown(outputs->name);
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = NULL;

        inputs->name = av_strdup("out");
        alloc_track_disown(inputs->name);
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0) {
            goto fail;
        }
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
            goto fail;
        }
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first. */
    for (i = 0; i < graph->nb_filters - nb_filters; i++) {
        FFSWAP(AVFilterContext *, graph->filters[i], graph->filters[i + nb_filters]);
    }

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    return ret;
}

int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void drop_resized_film_grain(AVFrame *frame) {
    static int warned = 0;
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
    const AVFilmGrainParams *fgp;

    if (!sd) {
        return;
    }
    fgp = (const AVFilmGrainParams *)sd->data;
    if (fgp->width <= 0 || fgp->height <= 0 ||
        (fgp->width == frame->width && fgp->height == frame->height)) {
        return;
    }
    av_frame_remove_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
    if (!warned) {
        warned = 1;
        log_verbose("Dropping film grain because the filters resized %dx%d to %dx%d.\n",
                    fgp->width, fgp->height, frame->width, frame->height);
    }
}

int video_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    int last_out_w = -1;
    int last_out_h = -1;
    AVRational last_out_sar = {0, 1};
    AVRational last_out_fr = {0, 0};
    int download_active = 0;
    int report_out_pending = 0;
    int crop_warned = 0;

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    thread_set_priority(SDL_THREAD_PRIORITY_NORMAL, "video decoder");

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0) {
            goto the_end;
        }
        if (!ret) {
            continue;
        }

        /* Downloading the frame below changes all three of these. */
        enum AVPixelFormat raw_format = frame->format;
        int raw_w = frame->width;
        int raw_h = frame->height;

        /* clang-format off */
        if (last_w != raw_w || last_h != raw_h ||
            last_format != raw_format ||
            last_serial != is->viddec.pkt_serial ||
            last_vfilter_idx != is->vfilter_idx) {
            /* clang-format on */
            const char *vfilters = vfilters_list ? vfilters_list[is->vfilter_idx] : NULL;
            int is_hw = frame->hw_frames_ctx != NULL;

            media_info_note_hw_frame(is_hw);

            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                goto the_end;
            }

            download_active = 0;
            if (is_hw) {
                int saved_level = av_log_get_level();
                av_log_set_level(AV_LOG_QUIET);
                ret = configure_video_filters(graph, is, vfilters, frame, 0);
                av_log_set_level(saved_level);
            } else {
                ret = configure_video_filters(graph, is, vfilters, frame, 0);
            }

            if (ret < 0 && is_hw) {
                avfilter_graph_free(&graph);
                graph = avfilter_graph_alloc();
                if (!graph) {
                    goto the_end;
                }
                hwframe_download_inplace(frame);
                download_active = 1;
                ret = configure_video_filters(graph, is, vfilters, frame, 0);
            }

            if (ret >= 0 && !frame->hw_frames_ctx &&
                filtergraph_output_oversize(is->out_video_filter)) {
                avfilter_graph_free(&graph);
                graph = avfilter_graph_alloc();
                if (!graph) {
                    goto the_end;
                }
                ret = configure_video_filters(graph, is, vfilters, frame, 1);
            }

            if (ret < 0) {
                SDL_Event event;
                SDL_zero(event);
                event.type = FF_QUIT_EVENT;
                event.user.code = FF_QUIT_REASON_ERROR;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = raw_w;
            last_h = raw_h;
            last_format = raw_format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
            report_out_pending = 1;
        } else if (download_active && frame->hw_frames_ctx) {
            hwframe_download_inplace(frame);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0) {
            goto the_end;
        }

        while (ret >= 0) {
            FrameData *fd;

            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    is->viddec.finished = is->viddec.pkt_serial;
                }
                ret = 0;
                break;
            }

            if (!frame->hw_frames_ctx &&
                (frame->crop_left || frame->crop_top ||
                 frame->crop_right || frame->crop_bottom)) {
                int crop_ret = av_frame_apply_cropping(frame, 0);

                if (crop_ret < 0 && !crop_warned) {
                    crop_warned = 1;
                    log_warn("Failed to apply frame cropping: %s.\n",
                             av_err2str(crop_ret));
                }
            }

            drop_resized_film_grain(frame);

            if (report_out_pending) {
                report_out_pending = 0;
                report_filter_output(filt_out, frame, &last_out_w, &last_out_h,
                                     &last_out_sar, &last_out_fr);
            }

            fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0) {
                is->frame_last_filter_delay = 0;
            }
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? LACHESIS_NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (is->videoq.serial != is->viddec.pkt_serial) {
                break;
            }
        }

        if (ret < 0) {
            goto the_end;
        }
    }
the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);

    return 0;
}

static VideoState *stream_open(const char *filename,
                               const AVInputFormat *iformat,
                               const char *archive_path,
                               const char *entry_name,
                               int from_playlist) {
    VideoState *is;
    int vol, vol_max_pct;

    is = av_mallocz(sizeof(VideoState));
    if (!is) {
        return NULL;
    }
    video_adopt_window_size(is);
    is->last_render_serial = -1;
    is->observed_pos = LACHESIS_NAN;
    SDL_SetAtomicInt(&is->seek_by_bytes, -1);
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    media_info_reset();
    av_freep(&window_title_path);
    window_title_path = make_default_window_title(filename, archive_path,
                                                  entry_name);
    is->ytdl_forced = !strncmp(filename, "ytdl://", 7);
    is->filename = av_strdup(filename);
    if (!is->filename) {
        goto fail;
    }
    is->open_started_us = av_gettime_relative();
    is->iformat = iformat;
    is->from_playlist = from_playlist;
    is->archive_path = NULL;
    is->entry_name = NULL;
    /* These must be set before read_thread is created. */
    if (archive_path && entry_name) {
        is->archive_path = av_strdup(archive_path);
        is->entry_name = av_strdup(entry_name);
        if (!is->archive_path || !is->entry_name) {
            goto fail;
        }
    }
    is->ytop = 0;
    is->xleft = 0;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) {
        goto fail;
    }
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0) {
        goto fail;
    }
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0) {
        goto fail;
    }

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0) {
        goto fail;
    }

    if (!(is->continue_read_thread = SDL_CreateCondition())) {
        goto fail;
    }
    if (!(is->pipeline_mutex = SDL_CreateMutex())) {
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->exact_seek_backoff_target = LACHESIS_NAN;
    is->audio_catchup_pts = LACHESIS_NAN;
    is->audio_catchup_serial = -1;
    is->audio_catchup_checked_serial = -1;
    is->pictq_last_serial = -1;
    is->decode_span_pts = LACHESIS_NAN;
    is->decode_span_serial = -1;

    is->last_av_diff = LACHESIS_NAN;
    is->start_playhead = LACHESIS_NAN;
    exact_seek_cancel(is);
    if (video_background) {
        int type = parse_video_background(
            video_background, is->render_params.video_background_color);

        av_assert0(type >= 0);
        is->render_params.video_background_type = type;
        is->render_params.video_background_explicit = 1;
    }
    vol_max_pct = allow_volume_boost ? VOLUME_BOOST_MAX_PCT : 100;
    is->audio_volume_max = allow_volume_boost
        ? (FFP_MIX_MAXVOLUME * VOLUME_BOOST_MAX_PCT + 50) / 100
        : FFP_MIX_MAXVOLUME;
    vol = av_clip(startup_volume, 0, vol_max_pct);
    vol = av_clip((FFP_MIX_MAXVOLUME * vol + 50) / 100, 0, is->audio_volume_max);
    is->audio_volume = vol;
    is->muted = global_muted;
    is->av_sync_type = av_sync_type;
    is->begin_paused = pause_next_stream;
    pause_next_stream = 0;
    degrade_init(is);
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) {
    fail:
        stream_close(is);
        return NULL;
    }

    return is;
}

static void playlist_skip_unreachable(void) {
    while (playlist_pos < playlist_size &&
           !playlist_entry_is_reachable(playlist_pos)) {
        playlist_pos++;
    }
    if (playlist_pos >= playlist_size) {
        exit_status = 1;
        do_exit(NULL);
    }
}

static VideoState *stream_open_playlist_entry(int pos) {
    const PlaylistEntry *e = playlist_get(pos);

    if (!e) {
        return NULL;
    }

    return stream_open(e->display_path, file_iformat, e->archive_path,
                       e->entry_name, e->from_playlist);
}

static int startup_window_flags(void) {
    int win_flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN;

    if (alwaysontop) {
        win_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }

    return win_flags;
}

int display_max_texture_size(void) {
    if (max_texture_size) {
        return max_texture_size > 0 ? max_texture_size : 0;
    }

    return renderer_max_texture_size(renderer);
}

static char *startup_window_title(void) {
    const PlaylistEntry *e;

    if (window_title) {
        return av_strdup(window_title);
    }
    if (window_title_auto) {
        return av_strdup(window_title_auto);
    }

    e = playlist_get(playlist_pos);
    if (e) {
        return make_default_window_title(e->display_path, e->archive_path,
                                         e->entry_name);
    }

    return make_default_window_title(input_filename, NULL, NULL);
}

static AVDictionary *build_renderer_options(void) {
    AVDictionary *dict = NULL;

    if (gpu_params) {
        if (av_dict_parse_string(&dict, gpu_params, "=", ":", 0) < 0) {
            fatal_quit("Failed to parse '%s'.\n", gpu_params);
        }
    }
    if (vulkan_swap_mode) {
        av_dict_set(&dict, "present_mode", vulkan_swap_mode, 0);
    }
    if (benchmark) {
        av_dict_set(&dict, "present_mode", "immediate", 0);
        av_dict_set(&dict, "benchmark", "1", 0);
    }
    if (no_shader_cache) {
        av_dict_set(&dict, "cache", "0", 0);
    }
    if (shader_cache_dir && !no_shader_cache) {
        av_dict_set(&dict, "cache_dir", shader_cache_dir, 0);
    }
    if (icc_profile) {
        av_dict_set(&dict, "icc_profile", icc_profile, 0);
    }
    if (icc_auto) {
        av_dict_set(&dict, "icc_auto", "1", 0);
    }
    if (no_display_hdr) {
        av_dict_set(&dict, "display_hdr", "0", 0);
    }
    if (max_glsl_version > 0) {
        char buf[16];

        snprintf(buf, sizeof(buf), "%d", max_glsl_version);
        av_dict_set(&dict, "max_glsl_version", buf, 0);
    }

    return dict;
}

static unsigned renderer_faulted_apis;

static void open_renderer(enum RendererApi api) {
    RendererOpenParams params = {0};
    AVDictionary *dict = build_renderer_options();
    char *title = startup_window_title();
    char why[512];
    int ret;

    params.title = title ? title : program_name;
    params.width = default_width;
    params.height = default_height;
    params.window_flags = startup_window_flags();
    params.api = api;
    params.exclude = renderer_faulted_apis;
    if (no_vulkan) {
        params.exclude |= 1u << RENDERER_API_VULKAN;
    }
    params.opt = dict;

    window_placed = 0;
    ret = renderer_open(&params, &window, &renderer, why, sizeof(why));
    av_dict_free(&dict);
    av_free(title);

    if (ret < 0) {
        const char *driver = SDL_GetCurrentVideoDriver();

        fatal_quit("Failed to create a window and a GPU renderer on the "
                   "\"%s\" video driver: %s.\n",
                   driver ? driver : "none", why);
    }

    if (view360_enabled() &&
        renderer_enable_360(renderer, view360_layout, view360_projection) < 0) {
        fatal_quit("Failed to enable the 360° shader!\n");
    }

    if (supersample_level != SUPERSAMPLE_OFF &&
        renderer_set_supersample(renderer, supersample_level) < 0) {
        fatal_quit("Failed to enable the supersample shader!\n");
    }

    present_update_display_mode();
    renderer_drop_present_feedback(renderer);
    update_screen_size();
    if (no_vsync_snap || benchmark || !renderer_is_vsync_blocked(renderer)) {
        present_disable_snap();
    } else {
        present_restore_snap();
    }
}

void render_fault_fallback(VideoState **pis) {
    double resume_at = LACHESIS_NAN;
    int keep_paused;

    if (!renderer) {
        return;
    }

    /* Avoid an infinite loop. */
    renderer_faulted_apis |= 1u << renderer_api(renderer);
    if (gpu_api != RENDERER_API_AUTO &&
        (renderer_faulted_apis & (1u << gpu_api))) {
        log_dead("The %s renderer faulted and there is nothing to fall back to.\n",
                 renderer_api_name(renderer));
        do_exit(*pis);
    }

    keep_paused = *pis && (*pis)->paused;
    renderer_quiesce(renderer, 1);
    if (*pis) {
        if (render_ever_ok && SDL_GetAtomicInt(&(*pis)->seek_by_bytes) <= 0) {
            resume_at = effective_playhead(*pis);
        }
        if (!stream_close(*pis)) {
            *pis = NULL;
            log_dead("The %s renderer faulted while a stream could not be "
                     "closed.\n",
                     renderer_api_name(renderer));
            do_exit(NULL);
        }
        *pis = NULL;
    }
    SDL_FlushEvents(FF_QUIT_EVENT, FF_QUIT_EVENT);

    if (!renderer_destroy(renderer)) {
        log_dead("The %s renderer is stuck presenting and cannot be replaced.\n",
                 renderer_api_name(renderer));
        do_exit(*pis);
    }
    av_freep(&renderer);
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    osd_invalidate_textures();

    open_renderer(gpu_api);
    present_reset();

    render_fail_streak = 0;
    render_ever_ok = 0;
    render_fault_event_sent = 0;

    playlist_reopen_current(pis, keep_paused, resume_at);
}

static void reread_from_playhead(VideoState *is) {
    double now;

    if (SDL_GetAtomicInt(&is->seek_by_bytes) > 0) {
        return;
    }
    now = effective_playhead(is);
    if (isnan(now)) {
        return;
    }

    stream_seek_exact(is, (int64_t)(now * AV_TIME_BASE));
}

void stream_cycle_channel(VideoState *is, int codec_type) {
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams;

    if (!ic) {
        return;
    }
    nb_streams = ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++) {
                if ((int)p->stream_index[start_index] == stream_index) {
                    break;
                }
            }
            if (start_index == nb_streams) {
                start_index = -1;
            }
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1) {
                return;
            }
            stream_index = 0;
        }
        if (stream_index == start_index) {
            return;
        }
        st = is->ic->streams[p ? (int)p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* Check that parameters are okay. */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0) {
                    goto the_end;
                }
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
the_end:
    if (p && stream_index != -1) {
        stream_index = p->stream_index[stream_index];
    }
    if (codec_type == AVMEDIA_TYPE_SUBTITLE && is->sub_ic) {
        close_external_subtitle(is);
    }
    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
    if (codec_type == AVMEDIA_TYPE_SUBTITLE && stream_index >= 0) {
        reread_from_playhead(is);
    }
}

void note_fullscreen_state(VideoState *is) {
    int fullscreen;
    int held;

    if (!window || !is || !window_placed) {
        return;
    }
    fullscreen = !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    if (is_fullscreen == fullscreen) {
        return;
    }
    log_verbose("The compositor put the window %s fullscreen.\n",
                fullscreen ? "into" : "out of");
    held = hold_output();
    is_fullscreen = fullscreen;
    if (!is_fullscreen && is->video_st && is->pictq.rindex_shown &&
        (!window_content_sized || window_resize)) {
        size_default_for_content(frame_queue_peek_last(&is->pictq));
    }
    update_screen_size();
    video_adopt_window_size(is);
    is->force_refresh = 1;
    present_update_display_mode();
    present_pacing_reset();
    drop_output(held);
}

void toggle_fullscreen(VideoState *is) {
    int held;

    if (!window) {
        return;
    }
    held = hold_output();
    is_fullscreen = !is_fullscreen;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        apply_window_geometry(default_width, default_height);
    }
    SDL_SyncWindow(window);
    update_screen_size();
    video_adopt_window_size(is);
    is->force_refresh = 1;
    present_update_display_mode();
    present_pacing_reset();
    drop_output(held);
}

static void input_poll(VideoState *is) {
    terminal_input_poll();
    single_poll();
    if (quit_signal) {
        do_exit(is);
    }
}

void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;

    quit_signal_polled = 1;
    refresh_window_title(is);
    SDL_PumpEvents();
    input_poll(is);
    while (SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) <= 0) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_HideCursor();
            cursor_hidden = 1;
        }
        if (!benchmark && remaining_time > 0.0) {
            uint64_t ns = (uint64_t)(remaining_time * 1000000000.0);
            if (remaining_time < REFRESH_RATE) {
                SDL_DelayPrecise(ns);
            } else {
                SDL_DelayNS(ns);
            }
        }
        remaining_time = REFRESH_RATE;
        ab_loop_check(is);
        if (is->audio_start_pending &&
            av_gettime_relative() - is->audio_start_pending_since > AUDIO_START_MAX_WAIT_US) {
            is->audio_start_pending = 0;
            audio_device_resume();
        }
        apply_present_feedback();
        refresh_display_info(is);
        if (renderer_take_image_repaint(renderer)) {
            is->force_refresh = 1;
        }
        if (!is->paused || is->step || is->force_refresh || !is->window_opened ||
            osd_wants_repaint(is, av_gettime_relative() / 1000000.0)) {
            video_refresh(is, &remaining_time);
        }
        finish_raise();
        SDL_PumpEvents();
        input_poll(is);
    }
}

int pause_to_carry(const VideoState *is) {
    if (!is) {
        return 0;
    }

    return is->is_still_image ? is->begin_paused : is->paused;
}

void playlist_switch(VideoState **pis, int new_pos) {
    if (new_pos < 0 || new_pos >= playlist_size) {
        return;
    }
    int keep_paused = pause_to_carry(*pis);
    stream_close(*pis);
    *pis = NULL;
    ab_loop_reset();
    reset_playback_speed();
    playlist_pos = new_pos;
    av_freep(&window_title_auto);
    pause_next_stream = keep_paused;
    VideoState *is = stream_open_playlist_entry(playlist_pos);
    if (!is) {
        log_dead("Failed to open playlist entry %d!\n", playlist_pos);
        do_exit(NULL);
    }
    print_current_file(is);
    *pis = is;
}

int playlist_close_current(VideoState **pis, double *resume_at) {
    VideoState *is = *pis;

    *resume_at = LACHESIS_NAN;
    if (!is) {
        return 1;
    }
    if (SDL_GetAtomicInt(&is->seek_by_bytes) <= 0) {
        *resume_at = effective_playhead(is);
    }
    *pis = NULL;

    return stream_close(is);
}

void playlist_reopen_current(VideoState **pis, int keep_paused, double resume_at) {
    pause_next_stream = keep_paused;
    *pis = stream_open_playlist_entry(playlist_pos);
    if (!*pis) {
        log_dead("Failed to open playlist entry %d!\n", playlist_pos);
        do_exit(NULL);
    }
    if (!isnan(resume_at) && resume_at > 0) {
        stream_seek(*pis, (int64_t)(resume_at * AV_TIME_BASE), 0, 0);
    }
    (*pis)->force_refresh = 1;
}

void playlist_drop_current(VideoState **pis, int keep_paused) {
    int removed = playlist_pos;
    playlist_remove_at(removed);

    if (playlist_size == 0) {
        do_exit(NULL);
    }

    int next = removed < playlist_size ? removed : playlist_size - 1;
    ab_loop_reset();
    reset_playback_speed();
    playlist_nav_dir = 1;
    playlist_pos = next;
    av_freep(&window_title_auto);
    pause_next_stream = keep_paused;
    VideoState *nis = stream_open_playlist_entry(playlist_pos);
    if (!nis) {
        log_dead("Failed to open playlist entry %d!\n", playlist_pos);
        do_exit(NULL);
    }
    print_current_file(nis);
    *pis = nis;
    nis->force_refresh = 1;
}

static int opt_input_file(void *optctx av_unused, const char *filename) {
    char **tmp;

    if (!strcmp(filename, "-")) {
        filename = "fd:";
    }

    tmp = av_realloc_array(input_args, n_input_args + 1, sizeof(*input_args));
    if (!tmp) {
        return AVERROR(ENOMEM);
    }
    input_args = tmp;
    input_args[n_input_args] = av_strdup(filename);
    if (!input_args[n_input_args]) {
        return AVERROR(ENOMEM);
    }
    n_input_args++;

    return 0;
}

static int add_input_file(const char *filename) {
    /* Keep input_filename pointing to the first file. */
    if (!input_filename) {
        input_filename = av_strdup(filename);
        if (!input_filename) {
            return AVERROR(ENOMEM);
        }
    }

    struct stat st;
    if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
        playlist_add_directory(filename);
        return 0;
    }

    return playlist_add_input(filename);
}

enum VideoDriverList {
    VIDEO_DRIVERS_ALL,
    VIDEO_DRIVERS_NO_WAYLAND,
    VIDEO_DRIVERS_PREFER_WAYLAND,
};

static int have_video_driver(const char *name) {
    int num = SDL_GetNumVideoDrivers();

    for (int i = 0; i < num; i++) {
        const char *have = SDL_GetVideoDriver(i);

        if (have && !strcmp(have, name)) {
            return 1;
        }
    }

    return 0;
}

static int want_video_driver(const char *name, enum VideoDriverList which,
                             int pass) {
    int wayland = !strcmp(name, "wayland");

    if (pass == 0) {
        return wayland && which == VIDEO_DRIVERS_PREFER_WAYLAND;
    }
    if (wayland) {
        return which == VIDEO_DRIVERS_ALL;
    }
    if (!strcmp(name, "dummy") || !strcmp(name, "evdev") ||
        !strcmp(name, "offscreen")) {
        return which == VIDEO_DRIVERS_ALL;
    }

    return 1;
}

static const char *video_driver_list(char *buf, size_t size,
                                     enum VideoDriverList which) {
    const char *sep = which == VIDEO_DRIVERS_ALL ? ", " : ",";
    int num = SDL_GetNumVideoDrivers();

    buf[0] = '\0';
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < num; i++) {
            const char *name = SDL_GetVideoDriver(i);
            size_t len = strlen(buf);

            if (!name || !want_video_driver(name, which, pass)) {
                continue;
            }
            if (len && av_strlcat(buf, sep, size) >= size) {
                buf[len] = '\0';
                return buf;
            }
            /* A truncated name is a driver that does not exist. */
            if (av_strlcat(buf, name, size) >= size) {
                buf[len] = '\0';
                return buf;
            }
        }
    }

    return buf;
}

static void pick_video_drivers(char *buf, size_t size) {
    const char *runtime_dir;
    enum VideoDriverList which;

    if (SDL_getenv("SDL_VIDEO_DRIVER") || SDL_getenv("SDL_VIDEODRIVER")) {
        return;
    }
    if (!have_video_driver("wayland") || !have_video_driver("x11")) {
        return;
    }

    runtime_dir = SDL_getenv("XDG_RUNTIME_DIR");
    which = runtime_dir && runtime_dir[0] &&
            (SDL_getenv("WAYLAND_DISPLAY") || SDL_getenv("WAYLAND_SOCKET"))
        ? VIDEO_DRIVERS_PREFER_WAYLAND
        : VIDEO_DRIVERS_NO_WAYLAND;

    if (*video_driver_list(buf, size, which)) {
        log_verbose("Asking SDL for these video drivers: %s.\n", buf);
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, buf);
    }
}

static const char *audio_driver_list(char *buf, size_t size) {
    int num = SDL_GetNumAudioDrivers();

    buf[0] = '\0';
    for (int i = 0; i < num; i++) {
        const char *name = SDL_GetAudioDriver(i);
        size_t len = strlen(buf);

        if (!name) {
            continue;
        }
        if (len && av_strlcat(buf, ", ", size) >= size) {
            buf[len] = '\0';
            return buf;
        }
        /* A truncated name is a driver that does not exist. */
        if (av_strlcat(buf, name, size) >= size) {
            buf[len] = '\0';
            return buf;
        }
    }

    return buf;
}

static void validate_options(void) {
    for (int i = 0; i < nb_vfilters; i++) {
        if (check_filtergraph(vfilters_list[i]) < 0) {
            fatal_quit("Invalid video filter \"%s\".\n", vfilters_list[i]);
        }
    }
    if (check_filtergraph(afilters_opt) < 0) {
        fatal_quit("Invalid audio filter \"%s\".\n", afilters_opt);
    }
    if (fps_convert < 0 || fps_convert > 480) {
        fatal_quit("-%s must be between 0 and 480.\n",
                   option_name(options, &fps_convert));
    }
    if (display_fps_override < 0 || display_fps_override > 1000) {
        fatal_quit("-%s must be between 0 and 1000.\n",
                   option_name(options, &display_fps_override));
    }
    validate_option_relations(options);
    if (audio_spdif_opt && audio_spdif_opt[0] &&
        !audio_spdif_names_known(audio_spdif_opt)) {
        log_warn("Unknown S/PDIF codec '%s'.\n", audio_spdif_opt);
    }
}

static void fatal_sdl_init(const char *subsystem) {
    char video[256];
    char audio[256];
    char err[256];

    snprintf(err, sizeof(err), "%s", SDL_GetError());
    video_driver_list(video, sizeof(video), VIDEO_DRIVERS_ALL);
    audio_driver_list(audio, sizeof(audio));

    fatal_quit("Could not initialize SDL %s: %s! Available video drivers: %s. "
               "Available audio drivers: %s.\n",
               subsystem, err, *video ? video : "none", *audio ? audio : "none");
}

int main(int argc, char **argv) {
    char drivers[256];
    int flags, ret;
    VideoState *is;

    alloc_track_init();

    setvbuf(stdout, NULL, _IOLBF, 0);

#if defined(_WIN32)
    win32_attach_console();
    win32_argv_to_utf8(&argc, &argv);
#endif

    terminal_output_init();
    init_dynload();

    log_init();
    validate_option_tables(options);
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_ERROR);
    parse_loglevel(argc, argv, options);
    parse_quiet(argc, argv, options);
    parse_allow_unsafe(argc, argv, options);
    parse_all_files(argc, argv, options);

#if LACHESIS_HAVE_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    /* The command line wins. */
    ret = load_config_file(NULL, options);
    if (ret < 0) {
        uninit_opts();
        alloc_track_complete();
        exit(1);
    }
    int nb_config_vfilters = nb_vfilters;

    ret = parse_options(NULL, argc, argv, options, opt_input_file);
    if (ret < 0) {
        uninit_opts();
        alloc_track_complete();
        exit(ret == AVERROR_EXIT ? 0 : 1);
    }

    if (nb_vfilters > nb_config_vfilters) {
        for (int i = 0; i < nb_config_vfilters; i++) {
            av_freep(&vfilters_list[i]);
        }
        nb_vfilters -= nb_config_vfilters;
        for (int i = 0; i < nb_vfilters; i++) {
            vfilters_list[i] = vfilters_list[i + nb_config_vfilters];
        }
    }

    validate_options();

    if (single_claim(input_args, n_input_args) == SINGLE_ROLE_HANDED_OFF) {
        uninit_opts();
        alloc_track_complete();
        exit(0);
    }

    for (int i = 0; i < n_input_args; i++) {
        if (add_input_file(input_args[i]) < 0) {
            fatal_quit("Could not add '%s' to the playlist.\n", input_args[i]);
        }
    }

    playlist_drop_character_devices(file_iformat != NULL);

    if (playlist_size == 0) {
        opt_version(NULL, NULL, NULL);
        playlist_report_filtered();
        fatal_quit("An input file must be specified.\n");
    }
    playlist_report_filtered();
    if (reverse_playlist) {
        playlist_reverse();
    }
    if (shuffle) {
        playlist_shuffle();
    }
    if (display_disable) {
        video_disable = 1;
    }
    if (benchmark) {
        audio_disable = 1;
    }

    playlist_pos = 0;
    playlist_skip_unreachable();

    flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (display_disable) {
        flags &= ~SDL_INIT_VIDEO;
    }
    if (!SDL_getenv("SDL_MUTE_CONSOLE_KEYBOARD")) {
        SDL_SetHint(SDL_HINT_MUTE_CONSOLE_KEYBOARD, "0");
    }
    SDL_SetAppMetadata(program_name, VERSION, program_name);
    pick_video_drivers(drivers, sizeof(drivers));
    if (!SDL_Init(flags)) {
        fatal_sdl_init(display_disable ? "events" : "video");
    }
    if (!audio_disable && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fatal_sdl_init("audio");
    }

    SDL_SetEventEnabled(SDL_EVENT_USER, false);

    if (!display_disable) {
        thread_set_priority(SDL_THREAD_PRIORITY_HIGH, "display");
    }

    terminal_input_init();

    if (start_windowed) {
        is_fullscreen = 0;
    }

    if (gpu_api_name) {
        if (!strcmp(gpu_api_name, "auto")) {
            gpu_api = RENDERER_API_AUTO;
        } else if (!strcmp(gpu_api_name, "vulkan")) {
            gpu_api = RENDERER_API_VULKAN;
        } else if (!strcmp(gpu_api_name, "opengl") || !strcmp(gpu_api_name, "gl")) {
            gpu_api = RENDERER_API_OPENGL;
        } else if (!strcmp(gpu_api_name, "d3d11") ||
                   !strcmp(gpu_api_name, "direct3d11")) {
            gpu_api = RENDERER_API_D3D11;
        } else {
            fatal_quit("Unknown GPU API '%s'.\n",
                       gpu_api_name);
        }
    } else if (no_vulkan) {
        gpu_api = RENDERER_API_OPENGL;
    }

    if (disable_autorotate) {
        autorotate = 0;
    }

    if (!display_disable) {
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
        init_default_window_size();
        if (enable_360sbs || enable_360tb || enable_360eq || enable_360eqtb) {
            view360_projection = (enable_360eq || enable_360eqtb)
                ? VIEW360_PROJECTION_SPHERE
                : VIEW360_PROJECTION_PANINI;
            view360_layout = (enable_360tb || enable_360eqtb)
                ? VIEW360_LAYOUT_TB
                : VIEW360_LAYOUT_FULL;
            sbs360_reset_view();
        }

        open_renderer(gpu_api);

        osd_init();
        subtitles_init();
        osd_set_info_provider(format_media_info);
        osd_set_stats_provider(format_playback_stats);
        osd_warmup();
    }

    normalize_init();
    alloc_track_setup_done();

    is = stream_open_playlist_entry(playlist_pos);
    if (!is) {
        do_exit(NULL);
    }

    print_current_file(is);

    event_loop(&is);

    /* Never returns. */
    return 0;
}
