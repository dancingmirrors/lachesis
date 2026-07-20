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

#if LACHESIS_HAVE_LIBPLACEBO
#include <libplacebo/config.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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
#include <libavutil/lfg.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/random_seed.h>
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
#include <SDL3_ttf/SDL_ttf.h>

#include <stdbool.h>
#include <strings.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define PATH_SEPARATOR '\\'
#else
#include <unistd.h>
#define PATH_SEPARATOR '/'
#endif

#include "lachesis_archive.h"
#include "lachesis_audio.h"
#include "lachesis_cmdutils.h"
#include "lachesis_demux.h"
#include "lachesis_filters.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_keys.h"
#include "lachesis_log.h"
#include "lachesis_network.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_rc.h"
#include "lachesis_renderer.h"
#include "lachesis_subtitles.h"
#include "lachesis_terminal.h"

const char program_name[] = "lachesis";
const int program_birth_year = 2003;

#define DECODE_BEHIND_LATCH_FRAMES 20
#define DECODE_RECOVER_FRAMES 120
#define CATCHUP_BEHIND_SECS 1.0
#define CATCHUP_COOLDOWN_US (18 * 1000000)
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define REFRESH_RATE 0.01

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static const char *input_filename;

static PlaylistEntry *playlist_entries = NULL;

static int pause_next_stream = 0;

int playlist_size = 0;
int playlist_pos = 0;
int playlist_nav_dir = 1;
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
enum Vk360Layout view360_layout = VK_360_LAYOUT_FULL;
float sbs360_yaw = 0.0f;
float sbs360_pitch = VIEW360_DEFAULT_PITCH;
float sbs360_hfov = VIEW360_DEFAULT_HFOV;

void sbs360_reset_view(void) {
    sbs360_yaw = view360_default_yaw(view360_layout);
    sbs360_pitch = VIEW360_DEFAULT_PITCH;
    sbs360_hfov = VIEW360_DEFAULT_HFOV;
}

SDL_Window *window;
SDL_Renderer *renderer;

const SDL_PixelFormat *renderer_texture_formats = NULL;

VkRenderer *vk_renderer;

double ab_loop_a = NAN;
double ab_loop_b = NAN;

int ab_loop_defining(void) {
    return !isnan(ab_loop_a) && isnan(ab_loop_b);
}

double playback_speed = 1.0;

#define PLAYBACK_SPEED_MIN 0.2
#define PLAYBACK_SPEED_MAX 2.0

const struct TextureFormatEntry sdl_texture_format_map[SDL_TEXTURE_FORMAT_MAP_SIZE] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_XRGB4444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_XRGB1555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_XBGR1555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_XRGB8888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_XBGR8888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
};

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

    /* XXX */
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

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
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

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) {
                    return -1;
                }

                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    /* Only the subtitle path passes a NULL frame. */
                    av_assert0(frame);
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        if (decoder_reorder_pts == -1) {
                            frame->pts = frame->best_effort_timestamp;
                        } else if (!decoder_reorder_pts) {
                            frame->pts = frame->pkt_dts;
                        }
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
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0) {
                    return -1;
                }
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
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
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
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
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

/* clang-format off */
int realloc_texture(SDL_Texture **texture, Uint32 new_format,
                    int new_width, int new_height,
                    SDL_BlendMode blendmode, int init_texture) {
    /* clang-format on */
    Uint32 format = SDL_PIXELFORMAT_UNKNOWN;
    float w = 0, h = 0;
    if (*texture) {
        SDL_PropertiesID props = SDL_GetTextureProperties(*texture);
        format = (Uint32)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN);
        SDL_GetTextureSize(*texture, &w, &h);
    }
    if (!*texture || new_width != (int)w || new_height != (int)h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture) {
            SDL_DestroyTexture(*texture);
        }
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height))) {
            return -1;
        }
        if (!SDL_SetTextureBlendMode(*texture, blendmode)) {
            return -1;
        }
        if (init_texture) {
            if (!SDL_LockTexture(*texture, NULL, &pixels, &pitch)) {
                return -1;
            }
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
    }

    return 0;
}

void calculate_display_rect(SDL_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

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

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    size_t i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32 ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 ||
        format == AV_PIX_FMT_BGR32_1) {
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map); i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(SDL_Texture **tex, AVFrame *frame) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    /* clang-format off */
    if (realloc_texture(tex,
                        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN
                            ? SDL_PIXELFORMAT_ARGB8888
                            : sdl_pix_fmt,
                        frame->width, frame->height, sdl_blendmode, 0) < 0) {
        /* clang-format on */
        return -1;
    }
    switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                       frame->data[1], frame->linesize[1],
                                       frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
            /* clang-format off */
            ret = SDL_UpdateYUVTexture(
                *tex, NULL,
                frame->data[0] + frame->linesize[0] * (frame->height - 1),
                -frame->linesize[0],
                frame->data[1] +
                    frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[1],
                frame->data[2] +
                    frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[2]);
            /* clang-format on */
        } else {
            return -1;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            /* clang-format off */
            ret = SDL_UpdateTexture(
                *tex, NULL,
                frame->data[0] + frame->linesize[0] * (frame->height - 1),
                -frame->linesize[0]);
            /* clang-format on */
        } else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }

    return ret ? 0 : -1;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
    /* XXX */
    (void)frame;
}

static void draw_video_background(VideoState *is) {
    const int tile_size = VIDEO_BACKGROUND_TILE_SIZE;
    SDL_Rect *rect = &is->render_params.target_rect;
    SDL_BlendMode blendMode;

    if (!SDL_GetTextureBlendMode(is->vid_texture, &blendMode) && blendMode == SDL_BLENDMODE_BLEND) {
        switch (is->render_params.video_background_type) {
        case VIDEO_BACKGROUND_TILES:
            SDL_SetRenderDrawColor(renderer, 237, 237, 237, 255);
            fill_rectangle(rect->x, rect->y, rect->w, rect->h);
            SDL_SetRenderDrawColor(renderer, 222, 222, 222, 255);
            for (int x = 0; x < rect->w; x += tile_size * 2) {
                fill_rectangle(rect->x + x, rect->y, FFMIN(tile_size, rect->w - x), rect->h);
            }
            for (int y = 0; y < rect->h; y += tile_size * 2) {
                fill_rectangle(rect->x, rect->y + y, rect->w, FFMIN(tile_size, rect->h - y));
            }
            SDL_SetRenderDrawColor(renderer, 237, 237, 237, 255);
            for (int y = 0; y < rect->h; y += tile_size * 2) {
                int h = FFMIN(tile_size, rect->h - y);
                for (int x = 0; x < rect->w; x += tile_size * 2) {
                    fill_rectangle(x + rect->x, y + rect->y, FFMIN(tile_size, rect->w - x), h);
                }
            }
            break;
        case VIDEO_BACKGROUND_COLOR: {
            const uint8_t *c = is->render_params.video_background_color;
            SDL_SetRenderDrawColor(renderer, c[0], c[1], c[2], c[3]);
            fill_rectangle(rect->x, rect->y, rect->w, rect->h);
            break;
        }
        case VIDEO_BACKGROUND_NONE:
            SDL_SetTextureBlendMode(is->vid_texture, SDL_BLENDMODE_NONE);
            break;
        }
    }
}

static void video_image_display(VideoState *is) {
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect *rect = &is->render_params.target_rect;

    vp = frame_queue_peek_last(&is->pictq);
    calculate_display_rect(rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);
    if (vk_renderer) {
        if (enable_360sbs) {
            vk_renderer_update_360(vk_renderer, sbs360_yaw, sbs360_pitch, sbs360_hfov);
        }
        is->render_params.disable_linear_scaling = is->render_low_quality;
        is->render_params.skip_anti_aliasing = is->render_low_quality;
        is->render_params.deinterlace = deinterlace;
        is->render_params.rotate = video_rotate;
        vk_renderer_display(vk_renderer, vp->frame, &is->render_params);
        return;
    }

    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (sp->sub.format != 0) {
                sp = NULL;
            } else if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    unsigned int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0) {
                        return;
                    }

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            return;
                        }
                        if (SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t *const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else {
                sp = NULL;
            }
        }
    }

    set_sdl_yuv_conversion_mode(vp->frame);

    if (!vp->uploaded) {
        if (upload_texture(&is->vid_texture, vp->frame) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    draw_video_background(is);
    SDL_FRect dst_rectf = {rect->x, rect->y, rect->w, rect->h};
    if (video_rotate == 90 || video_rotate == 270) {
        float cx = rect->x + rect->w / 2.0f;
        float cy = rect->y + rect->h / 2.0f;
        dst_rectf.w = rect->h;
        dst_rectf.h = rect->w;
        dst_rectf.x = cx - dst_rectf.w / 2.0f;
        dst_rectf.y = cy - dst_rectf.h / 2.0f;
    }
    SDL_RenderTextureRotated(renderer, is->vid_texture, NULL, &dst_rectf,
                             (double)video_rotate, NULL,
                             vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderTexture(renderer, is->sub_texture, NULL, &dst_rectf);
#else
        int i;
        double xratio = (double)rect->w / (double)sp->width;
        double yratio = (double)rect->h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect *)sp->sub.rects[i];
            SDL_FRect srcf = {sub_rect->x, sub_rect->y, sub_rect->w, sub_rect->h};
            SDL_FRect target = {.x = rect->x + sub_rect->x * xratio,
                                .y = rect->y + sub_rect->y * yratio,
                                .w = sub_rect->w * xratio,
                                .h = sub_rect->h * yratio};
            SDL_RenderTexture(renderer, is->sub_texture, &srcf, &target);
        }
#endif
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

static void stream_close(VideoState *is) {
    /* XXX */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);
    if (is->audio_read_tid) {
        SDL_WaitThread(is->audio_read_tid, NULL);
    }
    if (is->sub_read_tid) {
        SDL_WaitThread(is->sub_read_tid, NULL);
        is->sub_read_tid = NULL;
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
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    av_free(is->archive_path);
    av_free(is->entry_name);
    if (is->vid_texture) {
        SDL_DestroyTexture(is->vid_texture);
    }
    if (is->sub_texture) {
        SDL_DestroyTexture(is->sub_texture);
    }
    av_free(is);
}

void do_exit(VideoState *is) {
    if (is) {
        stream_close(is);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (vk_renderer) {
        vk_renderer_destroy(vk_renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    uninit_opts();
    for (int i = 0; i < nb_vfilters; i++) {
        av_freep(&vfilters_list[i]);
    }
    av_freep(&vfilters_list);
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&input_filename);
    for (int i = 0; i < playlist_size; i++) {
        av_free(playlist_entries[i].display_path);
        av_free(playlist_entries[i].archive_path);
        av_free(playlist_entries[i].entry_name);
    }
    av_freep(&playlist_entries);
    playlist_size = 0;
    for (int i = 0; i < n_pending_dirs; i++) {
        av_free(pending_dirs[i]);
    }
    av_freep(&pending_dirs);
    n_pending_dirs = 0;
    avformat_network_deinit();
    osd_uninit();
    TTF_Quit();
    SDL_Quit();
    exit(0);
}

static void sigterm_handler(int sig av_unused) {
    exit(123);
}

void set_default_window_size(int width, int height, AVRational sar) {
    SDL_Rect rect;
    int max_width, max_height;

    if (cmd_width || cmd_height) {
        max_width = cmd_width ? cmd_width : INT_MAX;
        max_height = cmd_height ? cmd_height : INT_MAX;
    } else {
        SDL_Rect display_bounds;
        if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &display_bounds)) {
            max_width = (int)(display_bounds.w * autofit_larger);
            max_height = (int)(display_bounds.h * autofit_larger);
        } else {
            max_width = max_height = INT_MAX;
        }
    }

    if (max_width == INT_MAX && max_height == INT_MAX) {
        max_height = height;
    }
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width = rect.w;
    default_height = rect.h;

    if (window && !is_fullscreen && !cmd_width && !cmd_height) {
        SDL_SetWindowSize(window, default_width, default_height);
    }
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

static int video_open(VideoState *is) {
    int w, h;

    w = cmd_width ? cmd_width : default_width;
    h = cmd_height ? cmd_height : default_height;

    if (!window_title) {
        const char *path = is->ytdl_source_url ? is->ytdl_source_url
                                               : is->filename;
        window_title = make_default_window_title(path, is->archive_path,
                                                 is->entry_name);
    }

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    SDL_SetWindowFullscreen(window, is_fullscreen);
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);

    if (window_title) {
        SDL_SetWindowTitle(window, window_title);
    }

    is->width = w;
    is->height = h;

    /* Make sure the damn thing is centered. */
    if (is_fullscreen && screen_width && screen_height) {
        is->width = screen_width;
        is->height = screen_height;
    }

    return 0;
}

static void video_display(VideoState *is) {
    if (!is->width) {
        video_open(is);
    }

    is->render_params.osd_pixels = NULL;
    osd_prepare_vulkan(is);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (is->video_st) {
        video_image_display(is);
    }
    osd_draw(is);
    SDL_RenderPresent(renderer);
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
    double sync_threshold, diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold) {
                delay = FFMAX(0, delay + diff);
            } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = delay + diff;
            } else if (diff >= sync_threshold) {
                delay = 2 * delay;
            }
        }
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

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) {
        check_external_clock_speed(is);
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
            if (!benchmark && time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (!benchmark && delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
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
                /* clang-format off */
                if (!benchmark && !is->step &&
                    (framedrop > 0 || playback_speed > 1.0 ||
                     (framedrop &&
                      get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) &&
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
                        if (sp->uploaded) {
                            unsigned int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch) {
                                        memset(pixels, 0, sub_rect->w << 2);
                                    }
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
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
    is->force_refresh = 0;
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
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

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
            log_warn("Failed to download hardware frame to system memory (%s).\n", av_err2str(ret));
        }
        av_frame_free(&sw);
        return;
    }

    if (!announced) {
        announced = 1;
        log_info("Copying decoded frames from the GPU to system memory.\n");
    }

    av_frame_copy_props(sw, frame);
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
        int64_t decode_us = av_gettime_relative() - decode_t0;

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (had_packets) {
            AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
            int64_t interval_us =
                (fr.num > 0 && fr.den > 0) ? (int64_t)(1000000.0 * fr.den / fr.num) : 0;
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

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
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
    int download_active = 0;

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

        enum AVPixelFormat raw_format = frame->format;

        /* clang-format off */
        if (last_w != frame->width || last_h != frame->height ||
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
            graph->nb_threads = filter_nbthreads;

            /* XXX: Needs more testing when scale_vulkan becomes more useful. */
            download_active = 0;
            if (is_hw) {
                int saved_level = av_log_get_level();
                av_log_set_level(AV_LOG_QUIET);
                ret = configure_video_filters(graph, is, vfilters, frame);
                av_log_set_level(saved_level);
            } else {
                ret = configure_video_filters(graph, is, vfilters, frame);
            }

            if (ret < 0 && is_hw) {
                avfilter_graph_free(&graph);
                graph = avfilter_graph_alloc();
                if (!graph) {
                    goto the_end;
                }
                graph->nb_threads = filter_nbthreads;
                hwframe_download_inplace(frame);
                download_active = 1;
                ret = configure_video_filters(graph, is, vfilters, frame);
            }

            if (ret < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.code = FF_QUIT_REASON_ERROR;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = raw_format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
            report_filter_output(is, filt_out, &last_out_w, &last_out_h, &last_out_sar);
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
                               const char *entry_name) {
    VideoState *is;
    int vol;

    is = av_mallocz(sizeof(VideoState));
    if (!is) {
        return NULL;
    }
    is->vfilter_idx = startup_vfilter_idx;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    media_info_reset();
    is->filename = av_strdup(filename);
    if (!is->filename) {
        goto fail;
    }
    is->iformat = iformat;
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

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
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
    }
    vol = av_clip(startup_volume, 0, 100);
    vol = av_clip(FFP_MIX_MAXVOLUME * vol / 100, 0, FFP_MIX_MAXVOLUME);
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

static VideoState *stream_open_playlist_entry(int pos) {
    if (pos < 0 || pos >= playlist_size) {
        return NULL;
    }
    PlaylistEntry *e = &playlist_entries[pos];
    VideoState *is = stream_open(e->display_path, file_iformat,
                                 e->archive_path, e->entry_name);
    if (!is) {
        return NULL;
    }

    return is;
}

void stream_cycle_channel(VideoState *is, int codec_type) {
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

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
    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}

void toggle_fullscreen(VideoState *is) {
    is_fullscreen = !is_fullscreen;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        SDL_SetWindowSize(window, default_width, default_height);
        is->width = default_width;
        is->height = default_height;
        is->force_refresh = 1;
    }
}

void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    terminal_input_poll();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_HideCursor();
            cursor_hidden = 1;
        }
        if (!benchmark && remaining_time > 0.0) {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        ab_loop_check(is);
        if (!is->paused || is->force_refresh) {
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
    window_title = NULL;
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
    av_free(playlist_entries[removed].display_path);
    av_free(playlist_entries[removed].archive_path);
    av_free(playlist_entries[removed].entry_name);
    memmove(&playlist_entries[removed], &playlist_entries[removed + 1],
            (playlist_size - removed - 1) * sizeof(*playlist_entries));
    playlist_size--;

    if (playlist_size == 0) {
        do_exit(NULL);
        return;
    }

    int next = removed < playlist_size ? removed : playlist_size - 1;
    ab_loop_reset();
    reset_playback_speed();
    playlist_nav_dir = 1;
    playlist_pos = next;
    window_title = NULL;
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

static int playlist_add_entry(const char *display_path,
                              const char *archive_path,
                              const char *entry_name) {
    PlaylistEntry *tmp = av_realloc_array(playlist_entries,
                                          playlist_size + 1,
                                          sizeof(*playlist_entries));
    if (!tmp) {
        return AVERROR(ENOMEM);
    }
    playlist_entries = tmp;
    PlaylistEntry *e = &playlist_entries[playlist_size];
    e->display_path = av_strdup(display_path);
    e->archive_path = archive_path ? av_strdup(archive_path) : NULL;
    e->entry_name = entry_name ? av_strdup(entry_name) : NULL;
    playlist_size++;

    return 0;
}

/* A Fisher-Yates shuffle. */
static void shuffle_playlist(void) {
    if (playlist_size < 2) {
        return;
    }

    AVLFG lfg;
    av_lfg_init(&lfg, av_get_random_seed());

    for (int i = playlist_size - 1; i > 0; i--) {
        int j = av_lfg_get(&lfg) % (unsigned)(i + 1);
        PlaylistEntry tmp = playlist_entries[i];
        playlist_entries[i] = playlist_entries[j];
        playlist_entries[j] = tmp;
    }
}

static void reverse_playlist_entries(void) {
    if (playlist_size < 2) {
        return;
    }

    for (int i = 0, j = playlist_size - 1; i < j; i++, j--) {
        PlaylistEntry tmp = playlist_entries[i];
        playlist_entries[i] = playlist_entries[j];
        playlist_entries[j] = tmp;
    }
}

static void expand_directory(const char *dir_path) {
    PlaylistEntry *entries = NULL;
    int count = 0;
    if (playlist_from_directory(dir_path, &entries, &count) != 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (is_supported_archive(entries[i].display_path)) {
            PlaylistEntry *arc = NULL;
            int narc = 0;
            if (playlist_from_archive(entries[i].display_path, &arc, &narc) == 0 && narc > 0) {
                for (int j = 0; j < narc; j++) {
                    playlist_add_entry(arc[j].display_path,
                                       arc[j].archive_path,
                                       arc[j].entry_name);
                    av_free(arc[j].display_path);
                    av_free(arc[j].archive_path);
                    av_free(arc[j].entry_name);
                }
                av_free(arc);
                av_free(entries[i].display_path);
                continue;
            }
            av_free(arc);
        }
        playlist_add_entry(entries[i].display_path, NULL, NULL);
        av_free(entries[i].display_path);
    }
    av_free(entries);
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

    if (is_supported_archive(filename)) {
        PlaylistEntry *entries = NULL;
        int count = 0;
        if (playlist_from_archive(filename, &entries, &count) == 0 && count > 0) {
            for (int i = 0; i < count; i++) {
                playlist_add_entry(entries[i].display_path,
                                   entries[i].archive_path,
                                   entries[i].entry_name);
                av_free(entries[i].display_path);
                av_free(entries[i].archive_path);
                av_free(entries[i].entry_name);
            }
            av_free(entries);
            return 0;
        }
        /* Fall through on error and try opening it as a regular file. */
    }

    return playlist_add_entry(filename, NULL, NULL);
}

int main(int argc, char **argv) {
    int flags, ret;
    VideoState *is;

#if defined(_WIN32)
    win32_argv_to_utf8(&argc, &argv);
#endif

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_ERROR);
    parse_loglevel(argc, argv, options);
    parse_quiet(argc, argv, options);

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

    if (playlist_size == 0 && n_pending_dirs > 0) {
        expand_directory(pending_dirs[0]);
    }
    for (int i = 0; i < n_pending_dirs; i++) {
        av_free(pending_dirs[i]);
    }
    av_freep(&pending_dirs);
    n_pending_dirs = 0;

    if (playlist_size == 0) {
        opt_version(NULL, NULL, NULL);
        fatal_quit("An input file must be specified.\n");
    }
    if (reverse_playlist) {
        reverse_playlist_entries();
    }
    if (shuffle) {
        shuffle_playlist();
    }
    if (display_disable) {
        video_disable = 1;
    }
    if (benchmark) {
        audio_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO;
    if (audio_disable) {
        flags &= ~SDL_INIT_AUDIO;
    }
    if (display_disable) {
        flags &= ~SDL_INIT_VIDEO;
    }
    if (!SDL_getenv("SDL_MUTE_CONSOLE_KEYBOARD")) {
        SDL_SetHint(SDL_HINT_MUTE_CONSOLE_KEYBOARD, "0");
    }
    if (!SDL_Init(flags)) {
        fatal_quit("Could not initialize SDL: %s!\n", SDL_GetError());
    }

    SDL_SetEventEnabled(SDL_EVENT_USER, false);

    terminal_input_init();

    if (start_windowed) {
        is_fullscreen = 0;
    }

    if (disable_vulkan) {
        enable_vulkan = 0;
    }

    if (!display_disable) {
        int win_flags = SDL_WINDOW_HIDDEN;
        if (is_fullscreen) {
            win_flags |= SDL_WINDOW_FULLSCREEN;
        }
        if (alwaysontop) {
            win_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        }
        if (borderless) {
            win_flags |= SDL_WINDOW_BORDERLESS;
        } else {
            win_flags |= SDL_WINDOW_RESIZABLE;
        }

        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
        if (hwaccel && (!strcmp(hwaccel, "none") || !strcmp(hwaccel, "no") || !strcmp(hwaccel, "off") || !strcmp(hwaccel, "0"))) {
            no_hwaccel = 1;
            hwaccel = NULL;
        }
        if (hwaccel && !no_hwaccel && !enable_vulkan && !disable_vulkan) {
            enable_vulkan = 1;
        }
        if (enable_360sbs && enable_360tb) {
            fatal_quit("-360-sbs and -360-tb are mutually exclusive.\n");
        }
        if (enable_360tb) {
            /* XXX */
            enable_360sbs = 1;
            view360_layout = VK_360_LAYOUT_TB;
        }
        if (enable_360sbs && !enable_vulkan) {
            fatal_quit("-360-sbs and -360-tb require Vulkan.\n");
        }
        if (enable_vulkan) {
            vk_renderer = vk_get_renderer();
            if (vk_renderer) {
                win_flags |= SDL_WINDOW_VULKAN;
            } else {
                log_warn("Your SDL version doesn't support Vulkan.\n");
                enable_vulkan = 0;
            }
        }
        window = SDL_CreateWindow(program_name, default_width, default_height, win_flags);
        if (!window) {
            fatal_quit("Failed to create window: %s!\n", SDL_GetError());
        }

        if (vk_renderer) {
            AVDictionary *dict = NULL;

            if (vulkan_params) {
                ret = av_dict_parse_string(&dict, vulkan_params, "=", ":", 0);
                if (ret < 0) {
                    fatal_quit("Failed to parse %s.\n", vulkan_params);
                }
            }
            if (vulkan_swap_mode) {
                av_dict_set(&dict, "present_mode", vulkan_swap_mode, 0);
            } else if (benchmark) {
                av_dict_set(&dict, "present_mode", "immediate", 0);
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
            ret = vk_renderer_create(vk_renderer, window, dict);
            av_dict_free(&dict);
            if (ret < 0) {
                fatal_quit("Failed to create Vulkan renderer!\n");
            }
            if (enable_360sbs) {
                sbs360_reset_view();
                ret = vk_renderer_enable_360(vk_renderer, view360_layout);
                if (ret < 0) {
                    fatal_quit("Failed to enable 360 shader!\n");
                }
            }
        } else {
            renderer = SDL_CreateRenderer(window, NULL);
            if (renderer) {
                SDL_SetRenderVSync(renderer, benchmark ? 0 : 1);
                renderer_texture_formats = (const SDL_PixelFormat *)
                    SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                                           SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL);
            }
            if (!renderer || !renderer_texture_formats ||
                renderer_texture_formats[0] == SDL_PIXELFORMAT_UNKNOWN) {
                fatal_quit("Failed to create window or renderer: %s!\n", SDL_GetError());
            }
        }

        if (TTF_Init()) {
            osd_init_fonts();
        }
        osd_set_info_provider(format_media_info);
        osd_set_stats_provider(format_playback_stats);

        {
            const char *initial_title = window_title;
            char *initial_title_alloc = NULL;
            if (!initial_title) {
                if (playlist_size > 0) {
                    PlaylistEntry *e = &playlist_entries[playlist_pos];
                    initial_title = initial_title_alloc =
                        make_default_window_title(e->display_path,
                                                  e->archive_path,
                                                  e->entry_name);
                } else {
                    initial_title = initial_title_alloc =
                        make_default_window_title(input_filename, NULL, NULL);
                }
            }
            if (initial_title) {
                SDL_SetWindowTitle(window, initial_title);
            }
            av_free(initial_title_alloc);
        }

        /* Show the window early so the swapchain is fully initialized. */
        SDL_ShowWindow(window);
        if (vk_renderer) {
            int vk_w, vk_h;
            SDL_GetWindowSizeInPixels(window, &vk_w, &vk_h);
            if (vk_w > 0 && vk_h > 0) {
                vk_renderer_resize(vk_renderer, vk_w, vk_h);
            }
        }
    }

    playlist_pos = 0;
    is = stream_open_playlist_entry(playlist_pos);
    if (!is) {
        do_exit(NULL);
    }

    print_current_file(is);

    event_loop(&is);

    /* Never returns. */
    return 0;
}
