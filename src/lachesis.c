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

#include "lachesis_archive.h"
#include "lachesis_audio.h"
#include "lachesis_demux.h"
#include "lachesis_equalizer.h"
#include "lachesis_filters.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_keys.h"
#include "lachesis_log.h"
#include "lachesis_network.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_playlist.h"
#include "lachesis_present.h"
#include "lachesis_rc.h"
#include "lachesis_renderer.h"
#include "lachesis_subtitles.h"
#include "lachesis_terminal.h"
#include "lachesis_view360.h"

const char program_name[] = "lachesis";
const int program_birth_year = 2003;

static void uninit_opts(void) {
    av_dict_free(&format_opts);
}

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

#define DECODE_BEHIND_LATCH_FRAMES 20
#define DECODE_RECOVER_FRAMES 120
#define CATCHUP_BEHIND_SECS 1.0
#define CATCHUP_COOLDOWN_US (18 * 1000000)
#define SEEK_STALL_SLACK 1.0
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1

#define AV_SYNC_SLEW_GAIN 0.1
#define AV_SYNC_SLEW_FACTOR 0.1
#define AV_SYNC_RESYNC_THRESHOLD 0.2
#define PRESENT_LEAD_MAX 0.004

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define REFRESH_RATE 0.01

#define OSD_ONLY_REFRESH_RATE (1.0 / 30.0)

#define CURSOR_HIDE_DELAY 1000000

#define AUDIO_START_MAX_WAIT_US (10 * 1000000)

#define USE_ONEPASS_SUBTITLE_RENDER 1

static const char *input_filename;

static int pause_next_stream = 0;

static char **pending_dirs = NULL;
static int n_pending_dirs = 0;
static int startup_vfilter_idx = 0;
int default_width = 640;
int default_height = 480;
int screen_width = 0;
int screen_height = 0;
float display_scale = 1.0f;
float display_pan_x = 0.0f;
float display_pan_y = 0.0f;
int lachesis_quiet;
int64_t cursor_last_shown;
int cursor_hidden = 0;
int deinterlace = 0;
int fatal_error_pending = 0;
enum View360Layout view360_layout = VIEW360_LAYOUT_OFF;
float sbs360_yaw = 0.0f;
float sbs360_pitch = VIEW360_DEFAULT_PITCH;
float sbs360_roll = 0.0f;
float sbs360_hfov = VIEW360_DEFAULT_HFOV;

void sbs360_reset_view(void) {
    sbs360_yaw = view360_default_yaw(view360_layout);
    sbs360_pitch = VIEW360_DEFAULT_PITCH;
    sbs360_roll = 0.0f;
    sbs360_hfov = VIEW360_DEFAULT_HFOV;
}

SDL_Window *window;

Renderer *renderer;

#define RENDER_FAULT_LIMIT 8
#define RENDER_FAULT_LIMIT_LATE 90
static int render_fail_streak;
static int render_ever_ok;
static int render_fault_event_sent;

double ab_loop_a = NAN;
double ab_loop_b = NAN;

int ab_loop_defining(void) {
    return !isnan(ab_loop_a) && isnan(ab_loop_b);
}

double playback_speed = 1.0;

#define PLAYBACK_SPEED_MIN 0.2
#define PLAYBACK_SPEED_MAX 2.0

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

void calculate_display_rect(SDL_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (pic_width < 1) {
        pic_width = 1;
    }
    if (pic_height < 1) {
        pic_height = 1;
    }

    if (video_rotate == 90 || video_rotate == 270) {
        int tmp = pic_width;
        pic_width = pic_height;
        pic_height = tmp;
        if (aspect_ratio.num > 0 && aspect_ratio.den > 0) {
            aspect_ratio = av_make_q(aspect_ratio.den, aspect_ratio.num);
        }
    }

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    if (!video_unscaled) {
        height = pic_height;
        width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
        if (width > scr_width || height > scr_height) {
            height = scr_height;
            width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
            if (width > scr_width) {
                width = scr_width;
                height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
            }
        }
    } else {
        height = scr_height;
        width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
        if (width > scr_width) {
            width = scr_width;
            height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
        }
    }
    width = (int64_t)(width * display_scale);
    height = (int64_t)(height * display_scale);
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;

    {
        int64_t margin_x = scr_width / 8;
        int64_t margin_y = scr_height / 8;
        int64_t max_pan_x = (width + scr_width) / 2 - margin_x;
        int64_t max_pan_y = (height + scr_height) / 2 - margin_y;
        if (max_pan_x < 0) {
            max_pan_x = 0;
        }
        if (max_pan_y < 0) {
            max_pan_y = 0;
        }
        display_pan_x = av_clipf(display_pan_x, (float)-max_pan_x, (float)max_pan_x);
        display_pan_y = av_clipf(display_pan_y, (float)-max_pan_y, (float)max_pan_y);
        x += (int64_t)display_pan_x;
        y += (int64_t)display_pan_y;
    }

    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX((int)width, 1);
    rect->h = FFMAX((int)height, 1);
}

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
        vp->pts < sp->pts + ((float)sp->sub.start_display_time / 1000)) {
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
    }

    if (!is->sub_rgba) {
        return;
    }
    is->render_params.sub_pixels = is->sub_rgba;
    is->render_params.sub_width = is->sub_rgba_w;
    is->render_params.sub_height = is->sub_rgba_h;
    is->render_params.sub_stride = is->sub_rgba_w * 4;
}

static void video_update_target_rect(VideoState *is) {
    SDL_Rect *rect = &is->render_params.target_rect;

    if (is->video_st) {
        Frame *vp = frame_queue_peek_last(&is->pictq);
        int rotated = video_rotate == 90 || video_rotate == 270;

        calculate_display_rect(rect, is->xleft, is->ytop, is->width, is->height,
                               vp->width, vp->height, vp->sar);
        is->render_storage_w = rotated ? vp->height : vp->width;
        is->render_storage_h = rotated ? vp->width : vp->height;
        return;
    }

    int bw = 0, bh = 0;

    SDL_GetWindowSizeInPixels(window, &bw, &bh);
    if (bw <= 0 || bh <= 0) {
        bw = is->width;
        bh = is->height;
    }
    *rect = (SDL_Rect){0, 0, bw, bh};
    is->render_storage_w = is->render_storage_h = 0;
}

void video_prepare_overlays(VideoState *is) {
    is->render_params.osd_pixels = NULL;
    is->render_params.sub_pixels = NULL;
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
    int ret;

    if (view360_enabled()) {
        renderer_update_360(renderer, sbs360_yaw, sbs360_pitch, sbs360_roll, sbs360_hfov);
    }
    is->render_params.still_image = is->is_still_image;
    is->render_params.disable_linear_scaling = is->render_low_quality;
    is->render_params.skip_anti_aliasing = is->render_low_quality;
    is->render_params.deinterlace = deinterlace;
    is->render_params.rotate = video_rotate;
    is->render_params.next_frame = NULL;
    is->render_params.reset_history = vp->serial != is->last_render_serial;
    is->last_render_serial = vp->serial;
    EqualizerValues eq = equalizer_get();
    is->render_params.eq_brightness = eq.brightness;
    is->render_params.eq_gamma = eq.gamma;
    is->render_params.eq_contrast = eq.contrast;
    if (deinterlace == DEINTERLACE_YADIF &&
        frame_queue_nb_remaining(&is->pictq) > 0) {
        Frame *nextvp = frame_queue_peek(&is->pictq);
        if (nextvp != vp) {
            is->render_params.next_frame = nextvp->frame;
        }
    }

    ret = renderer_display(renderer, vp->frame, &is->render_params);
    if (ret == AVERROR(ERANGE)) {
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

static int reader_abandoned;

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
    reader_abandoned = 1;
}

static void stream_close(VideoState *is) {
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
        return;
    }

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
    SDL_DestroyCondition(is->continue_read_thread);
    SDL_DestroyMutex(is->pipeline_mutex);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    av_free(is->archive_path);
    av_free(is->entry_name);
    av_freep(&is->sub_rgba);
    av_free(is);
}

av_noreturn void do_exit(VideoState *is) {
    if (is) {
        stream_close(is);
    }
    if (renderer) {
        renderer_destroy(renderer);
        av_freep(&renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    if (reader_abandoned) {
        terminal_restore_now();
        _Exit(0);
    }
    uninit_opts();
    for (int i = 0; i < nb_vfilters; i++) {
        av_freep(&vfilters_list[i]);
    }
    av_freep(&vfilters_list);
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&hwaccel);
    av_freep(&window_title);
    av_freep(&window_title_auto);
    av_freep(&input_filename);
    playlist_clear();
    for (int i = 0; i < n_pending_dirs; i++) {
        av_free(pending_dirs[i]);
    }
    av_freep(&pending_dirs);
    n_pending_dirs = 0;
    avformat_network_deinit();
    subtitles_uninit();
    osd_uninit();
    SDL_Quit();
    exit(0);
}

static void sigterm_handler(int sig av_unused) {
    terminal_restore_now();
    _Exit(123);
}

/* XXX */
static float window_points_scale(void) {
    float density = window ? SDL_GetWindowPixelDensity(window)
                           : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

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

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0) {
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

static void size_default_for_content(const Frame *vp) {
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

/* Tracked so we don't hear 0x0 complaints. */
static float pinned_aspect;

static void pin_window_aspect(float aspect) {
    if (pinned_aspect == aspect) {
        return;
    }
    pinned_aspect = aspect;
    SDL_SetWindowAspectRatio(window, aspect, aspect);
}

static void apply_window_geometry(int w, int h) {
    float aspect = (w > 0 && h > 0 && !is_fullscreen &&
                    video_rotate == window_rotate)
        ? (float)w / (float)h
        : 0.0f;

    if (pinned_aspect != aspect) {
        pin_window_aspect(0.0f);
    }
    SDL_SetWindowSize(window, w, h);
    pin_window_aspect(aspect);
    if (!SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED)) {
        SDL_ClearError();
    }
}

static void video_follow_content_size(VideoState *is) {
    int turned;
    int w, h;
    Frame *vp;

    if (!is->video_st) {
        return;
    }

    vp = frame_queue_peek_last(&is->pictq);
    turned = video_rotate != window_rotate;
    if (video_rotate == noted_rotate && content_size_is_current(vp)) {
        return;
    }
    noted_rotate = video_rotate;
    note_content_size(vp);

    window_size_for_content(vp->width, vp->height, vp->sar, window_rotate, &w,
                            &h);

    if (turned) {
        pin_window_aspect(0.0f);
    }

    if (w == default_width && h == default_height) {
        return;
    }
    default_width = w;
    default_height = h;
    if (is_fullscreen) {
        return;
    }
    apply_window_geometry(default_width, default_height);
    SDL_SyncWindow(window);
    update_screen_size();
    video_adopt_window_size(is);
}

static int window_placed;

static int video_open(VideoState *is) {

    if (!window_title && !window_title_auto) {
        const char *path = is->ytdl_source_url ? is->ytdl_source_url
                                               : is->filename;
        window_title_auto = make_default_window_title(path, is->archive_path,
                                                      is->entry_name);
    }

    if (!window_placed) {
        window_placed = 1;
        window_rotate = noted_rotate = video_rotate;
        if (is->video_st) {
            size_default_for_content(frame_queue_peek_last(&is->pictq));
        }
        SDL_SetWindowFullscreen(window, is_fullscreen);
        apply_window_geometry(default_width, default_height);
        SDL_ShowWindow(window);
    } else {
        video_follow_content_size(is);
    }
    SDL_SyncWindow(window);
    present_update_display_mode();

    const char *title = window_title ? window_title : window_title_auto;
    if (title) {
        SDL_SetWindowTitle(window, title);
    }

    update_screen_size();
    if (!video_adopt_window_size(is)) {
        float scale = window_points_scale();

        is->width = FFMAX((int)lrintf(default_width * scale), 1);
        is->height = FFMAX((int)lrintf(default_height * scale), 1);
    }

    return 0;
}

static void video_display(VideoState *is) {
    if (!renderer) {
        return;
    }

    if (!is->window_opened) {
        is->window_opened = 1;
        video_open(is);
    } else {
        video_follow_content_size(is);
    }

    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_OCCLUDED)) {
        return;
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
        is->render_params.deinterlace = DEINTERLACE_OFF;
        renderer_display_blank(renderer, &is->render_params);
    }

    {
        const RenderParams *rp = &is->render_params;

        if (rp->present_source != PRESENT_SOURCE_SWAP) {
            present_note_present(rp->present_done_us);
            if (rp->present_display_us > 0) {
                present_feedback_display(rp->present_source,
                                         rp->present_display_us,
                                         rp->present_refresh_us);
            }
        } else if (rp->present_done_us > 0) {
            present_feedback(rp->present_done_us - rp->present_block_us,
                             rp->present_done_us);
        }
    }

    if (is->audio_start_pending) {
        is->audio_start_pending = 0;
        audio_device_resume();
    }
}

double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial) {
        return NAN;
    }
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
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
    set_clock(c, NAN, -1);
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

/* Fall back to the external clock so we can advance when there's no more audio. */
double effective_playhead(VideoState *is) {
    double pos = get_master_clock(is);

    if (is->seek_flags & AVSEEK_FLAG_BYTE) {
        return pos;
    }

    double target = (double)is->seek_pos / AV_TIME_BASE;
    if (isnan(pos) || pos < target - SEEK_STALL_SLACK) {
        pos = target;
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

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes) {
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes) {
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        }
        is->seek_req = 1;
        SDL_SignalCondition(is->continue_read_thread);
    }
}

static void stream_toggle_pause(VideoState *is) {
    is->start_pause_pending = 0;
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
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
    ab_loop_a = NAN;
    ab_loop_b = NAN;
}

void ab_loop_toggle(VideoState *is) {
    char a_buf[32], b_buf[32];
    osd_show_position();
    double pos = get_master_clock(is);
    if (isnan(pos)) {
        pos = (double)is->seek_pos / AV_TIME_BASE;
    }
    if (isnan(pos) || pos < 0) {
        pos = 0;
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
        ab_loop_fmt_time(ab_loop_a, a_buf, sizeof(a_buf));
        ab_loop_fmt_time(ab_loop_b, b_buf, sizeof(b_buf));
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

void set_playback_speed(double speed) {
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
    playback_speed = speed;
    audio_speed_serial++;
    osd_show_message("Speed: %d%%", (int)lrint(playback_speed * 100.0));
}

static void reset_playback_speed(void) {
    playback_speed = 1.0;
    audio_speed_serial++;
}

void step_to_next_frame(VideoState *is) {
    if (is->paused) {
        stream_toggle_pause(is);
    }
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is) {
    double diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        is->last_av_diff = isnan(diff) ? NAN : -diff;
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
                delay = delay + diff;
            }
        }
    } else {
        is->last_av_diff = NAN;
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

static void video_refresh(void *opaque, double *remaining_time) {
    VideoState *is = opaque;
    double time;
    int keep_refreshing = 0;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) {
        check_external_clock_speed(is);
    }

    if (!display_disable && SDL_GetAtomicInt(&is->streams_selected) &&
        !is->video_st) {
        double now = av_gettime_relative() / 1000000.0;
        int want = osd_active(is);

        if (is->force_refresh) {
            is->audio_only_clean = 0;
        }

        if (want || !is->audio_only_clean) {
            double next = is->audio_only_last_draw + OSD_ONLY_REFRESH_RATE;
            if (now >= next) {
                video_display(is);
                is->audio_only_last_draw = now;
                is->audio_only_clean = !want;
                next = now + OSD_ONLY_REFRESH_RATE;
            } else {
                keep_refreshing = 1;
            }
            *remaining_time = FFMIN(*remaining_time, FFMAX(next - now, 0.0));
        }
    }

    if (is->video_st) {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            if (is->videoq.nb_packets > 0) {
                *remaining_time = FFMIN(*remaining_time, 0.002);
            }
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial) {
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            if (is->paused) {
                goto display;
            }

            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is) / playback_speed;

            time = av_gettime_relative() / 1000000.0;
            if (!benchmark) {
                double ideal = is->frame_timer + delay;
                double target = present_snap(ideal, time);
                double lead = 0;
                if (target != ideal) {
                    lead = FFMIN(PRESENT_LEAD_MAX, present_vsync_sec() * 0.25);
                }
                if (time < target - lead) {
                    *remaining_time = FFMIN(target - lead - time, *remaining_time);
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
                         (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
                        (sp2 &&
                         is->vidclk.pts >
                             (sp2->pts +
                              ((float)sp2->sub.start_display_time / 1000)))) {
                        /* clang-format on */
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused) {
                stream_toggle_pause(is);
            }
        }
    display:
        if (!display_disable && is->force_refresh && is->pictq.rindex_shown) {
            video_display(is);
        }
    }
    is->force_refresh = keep_refreshing;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq))) {
        return -1;
    }

    vp->sar = src_frame->sample_aspect_ratio;

    frame_visible_size(src_frame, &vp->width, &vp->height);
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

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

void apply_degraded_decode(AVCodecContext *avctx) {
    avctx->skip_loop_filter = AVDISCARD_ALL;
    avctx->skip_frame = AVDISCARD_NONREF;
}
static void enable_degraded_decode(VideoState *is) {
    if (is->decode_degraded) {
        return;
    }
    is->decode_degraded = 1;
    is->render_low_quality = 1;
    if (is->viddec.avctx) {
        apply_degraded_decode(is->viddec.avctx);
    }
    if (!is->degraded_warned) {
        is->degraded_warned = 1;
        log_warn("Degraded decoding engaged. Quality will suffer.\n");
    } else {
        log_verbose("Degraded decoding engaged.\n");
    }
}

static void disable_degraded_decode(VideoState *is) {
    if (!is->decode_degraded) {
        return;
    }
    is->decode_degraded = 0;
    is->render_low_quality = 0;
    is->decode_behind_streak = 0;
    is->decode_recover_streak = 0;
    if (is->viddec.avctx) {
        is->viddec.avctx->skip_loop_filter = AVDISCARD_DEFAULT;
        is->viddec.avctx->skip_frame = AVDISCARD_DEFAULT;
    }
    log_verbose("Degraded decoding disengaged.\n");
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
        double dpts = NAN;
        int64_t decode_us = av_gettime_relative() - decode_t0 - is->viddec.wait_us;
        if (decode_us < 0) {
            decode_us = 0;
        }

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
        int64_t interval_us =
            (fr.num > 0 && fr.den > 0) ? (int64_t)(1000000.0 * fr.den / fr.num) : 0;

        if (had_packets) {
            if (interval_us > 0 && decode_us > interval_us) {
                if (is->decode_behind_streak < DECODE_BEHIND_LATCH_FRAMES) {
                    is->decode_behind_streak++;
                }
            } else if (is->decode_behind_streak > 0) {
                is->decode_behind_streak--;
            }
            if (is->decode_behind_streak >= DECODE_BEHIND_LATCH_FRAMES) {
                enable_degraded_decode(is);
            } else if (is->decode_degraded) {
                double m = get_master_clock(is);
                double v = get_clock(&is->vidclk);
                int have_headroom = interval_us > 0 && decode_us * 3 < interval_us;
                int in_sync = isnan(m) || isnan(v) ||
                    fabs(m - v) < AV_SYNC_THRESHOLD_MAX;
                if (have_headroom && in_sync) {
                    if (++is->decode_recover_streak >= DECODE_RECOVER_FRAMES) {
                        disable_degraded_decode(is);
                    }
                } else {
                    is->decode_recover_streak = 0;
                }
            }
            if (is->decode_degraded && skip_to_keyframe && !av_sync_type_explicit &&
                is->audio_st && is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
                double m = get_master_clock(is);
                double v = get_clock(&is->vidclk);
                int64_t now = av_gettime_relative();
                if (!isnan(m) && !isnan(v) && m - v > CATCHUP_BEHIND_SECS &&
                    !is->seek_req &&
                    now - is->last_catchup_us > CATCHUP_COOLDOWN_US) {
                    is->last_catchup_us = now;
                    log_warn(
                        "Video decoder can't keep up (%.1f ms/frame versus %.1f ms "
                        "real time). Taking evasive maneuvers.\n",
                        decode_us / 1000.0, interval_us / 1000.0);
                    stream_seek(is, (int64_t)(m * AV_TIME_BASE),
                                (int64_t)((m - v) * AV_TIME_BASE), 0);
                }
            }
        }

        if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                double slack = interval_us > 0 ? interval_us / 1000000.0
                                               : AV_SYNC_THRESHOLD_MIN;
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < -slack &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets &&
                    frame_queue_nb_remaining(&is->pictq) > 0) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
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
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = NULL;

        inputs->name = av_strdup("out");
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
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
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
    int vol;

    is = av_mallocz(sizeof(VideoState));
    if (!is) {
        return NULL;
    }
    is->vfilter_idx = startup_vfilter_idx;
    video_adopt_window_size(is);
    is->last_render_serial = -1;
    SDL_SetAtomicInt(&is->seek_by_bytes, -1);
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    media_info_reset();
    is->filename = av_strdup(filename);
    if (!is->filename) {
        goto fail;
    }
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
    is->audio_catchup_pts = NAN;
    is->audio_catchup_serial = -1;
    is->audio_catchup_checked_serial = -1;
    is->pictq_last_serial = -1;
    is->last_av_diff = NAN;
    if (video_background) {
        if (!strcmp(video_background, "none")) {
            is->render_params.video_background_type = VIDEO_BACKGROUND_NONE;
        } else if (strcmp(video_background, "tiles")) {
            /* Any other value is parsed as a color. */
            if (av_parse_color(is->render_params.video_background_color, video_background, -1, NULL) >= 0) {
                is->render_params.video_background_type = VIDEO_BACKGROUND_COLOR;
            } else {
                goto fail;
            }
        }
        is->render_params.video_background_explicit = 1;
    }
    int vol_max_pct = allow_volume_boost ? VOLUME_BOOST_MAX_PCT : 100;
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

    pinned_aspect = 0.0f;
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

    if (view360_enabled() && renderer_enable_360(renderer, view360_layout) < 0) {
        fatal_quit("Failed to enable the 360° shader!\n");
    }

    present_update_display_mode();
    update_screen_size();
    if (no_vsync_snap || benchmark || !renderer_is_vsync_blocked(renderer)) {
        present_disable_snap();
    } else {
        present_restore_snap();
    }
}

void render_fault_fallback(VideoState **pis) {
    double resume_at = NAN;
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
    if (*pis) {
        if (render_ever_ok && SDL_GetAtomicInt(&(*pis)->seek_by_bytes) <= 0) {
            resume_at = effective_playhead(*pis);
        }
        stream_close(*pis);
        *pis = NULL;
    }
    SDL_FlushEvents(FF_QUIT_EVENT, FF_QUIT_EVENT);

    renderer_destroy(renderer);
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

    pause_next_stream = keep_paused;
    *pis = stream_open_playlist_entry(playlist_pos);
    if (!*pis) {
        log_dead("Failed to open playlist entry %d!\n", playlist_pos);
        do_exit(NULL);
    }
    if (!isnan(resume_at) && resume_at > 0) {
        stream_seek(*pis, (int64_t)(resume_at * AV_TIME_BASE), 0, 0);
    }
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
}

void toggle_fullscreen(VideoState *is) {
    if (!window) {
        return;
    }
    is_fullscreen = !is_fullscreen;
    if (is_fullscreen) {
        pin_window_aspect(0.0f);
    }
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        apply_window_geometry(default_width, default_height);
        SDL_SyncWindow(window);
        update_screen_size();
        video_adopt_window_size(is);
        is->force_refresh = 1;
    }
    present_update_display_mode();
    present_reset();
}

void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    terminal_input_poll();
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
        if (!is->paused || is->force_refresh || !is->window_opened) {
            video_refresh(is, &remaining_time);
        }
        {
            int64_t now_ms = (int64_t)SDL_GetTicks();
            int64_t osd_until = osd_visible_until();
            if (osd_until > now_ms) {
                double osd_remaining = (osd_until - now_ms) / 1000.0;
                if (osd_remaining < remaining_time) {
                    remaining_time = osd_remaining;
                }
                if (is->paused) {
                    is->force_refresh = 1;
                }
            } else if (osd_until > 0) {
                if (is->paused) {
                    is->force_refresh = 1;
                }
                osd_reset_timers();
            }
        }
        SDL_PumpEvents();
        terminal_input_poll();
    }
}

void playlist_switch(VideoState **pis, int new_pos) {
    if (new_pos < 0 || new_pos >= playlist_size) {
        return;
    }
    int keep_paused = (*pis)->paused;
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

void playlist_remove_current(VideoState **pis, int keep_paused) {
    stream_close(*pis);
    *pis = NULL;

    int removed = playlist_pos;
    playlist_remove_at(removed);

    if (playlist_size == 0) {
        do_exit(NULL);
        return;
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
    if (!strcmp(filename, "-")) {
        filename = "fd:";
    }

    /* Keep input_filename pointing to the first file. */
    if (!input_filename) {
        input_filename = av_strdup(filename);
        if (!input_filename) {
            return AVERROR(ENOMEM);
        }
    }

    struct stat st;
    if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
        char **tmp = av_realloc_array(pending_dirs, n_pending_dirs + 1,
                                      sizeof(*pending_dirs));
        if (!tmp) {
            return AVERROR(ENOMEM);
        }
        pending_dirs = tmp;
        pending_dirs[n_pending_dirs] = av_strdup(filename);
        if (!pending_dirs[n_pending_dirs]) {
            return AVERROR(ENOMEM);
        }
        n_pending_dirs++;
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

#if defined(_WIN32)
    win32_attach_console();
    win32_argv_to_utf8(&argc, &argv);
#endif

    init_dynload();

    log_init();
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
    load_config_file(NULL, options);
    int nb_config_vfilters = nb_vfilters;

    ret = parse_options(NULL, argc, argv, options, opt_input_file);
    if (ret < 0) {
        exit(ret == AVERROR_EXIT ? 0 : 1);
    }

    /* XXX */
    if (nb_vfilters > nb_config_vfilters) {
        startup_vfilter_idx = nb_config_vfilters;
    }

    for (int i = 0; i < nb_vfilters; i++) {
        if (check_filtergraph(vfilters_list[i]) < 0) {
            fatal_quit("Invalid video filter \"%s\".\n",
                       vfilters_list[i]);
        }
    }
    if (check_filtergraph(afilters_opt) < 0) {
        fatal_quit("Invalid audio filter \"%s\".\n",
                   afilters_opt);
    }
    if (fps_convert < 0 || fps_convert > 480) {
        fatal_quit("-r must be between 0 and 480.\n");
    }
    if (display_fps_override < 0 || display_fps_override > 1000) {
        fatal_quit("-display-fps must be between 0 and 1000.\n");
    }

    if (playlist_size == 0 && n_pending_dirs > 0) {
        playlist_add_directory(pending_dirs[0]);
    }
    for (int i = 0; i < n_pending_dirs; i++) {
        av_free(pending_dirs[i]);
    }
    av_freep(&pending_dirs);
    n_pending_dirs = 0;

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
        if (hwaccel && (!strcmp(hwaccel, "none") || !strcmp(hwaccel, "no") || !strcmp(hwaccel, "off") || !strcmp(hwaccel, "0"))) {
            no_hwaccel = 1;
            av_freep(&hwaccel);
        }
        if (enable_360sbs && enable_360tb) {
            fatal_quit("-360-sbs and -360-tb are mutually exclusive.\n");
        }
        if (enable_360sbs || enable_360tb) {
            view360_layout = enable_360tb ? VIEW360_LAYOUT_TB : VIEW360_LAYOUT_FULL;
            sbs360_reset_view();
        }

        open_renderer(gpu_api);

        osd_init();
        subtitles_init();
        osd_set_info_provider(format_media_info);
        osd_set_stats_provider(format_playback_stats);
        osd_warmup();
    }

    is = stream_open_playlist_entry(playlist_pos);
    if (!is) {
        do_exit(NULL);
    }

    print_current_file(is);

    event_loop(&is);

    /* Never returns. */
    return 0;
}
