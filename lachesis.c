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

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/fifo.h>
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
#include <SDL3_ttf/SDL_ttf.h>

#include <stdbool.h>
#include <strings.h>
#include <sys/stat.h>

#include "lachesis_archive.h"
#include "lachesis_cmdutils.h"
#include "lachesis_osd_font.h"
#include "lachesis_renderer.h"

const char program_name[] = "lachesis";
const int program_birth_year = 2003;

static void fatal_quit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
#define SDL_VOLUME_STEP (0.75)
#define FFP_MIX_MAXVOLUME 128

#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define AUDIO_DIFF_AVG_NB 20

#define REFRESH_RATE 0.01

/* XXX: We assume that a decoded and resampled frame fits into this buffer. */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_Mutex *mutex;
    SDL_Condition *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int *queue_serial;
} Clock;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;
    double duration;
    int64_t pos;
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_Mutex *mutex;
    SDL_Condition *cond;
    PacketQueue *pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* XXX */
};

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_Condition *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
    SDL_Thread *read_tid;
    const AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum;
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size;
    unsigned int audio_buf1_size;
    int audio_buf_index;
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
    struct AudioParams audio_filter_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    enum ShowMode {
        SHOW_MODE_NONE = -1,
        SHOW_MODE_VIDEO = 0,
        SHOW_MODE_WAVES,
        SHOW_MODE_RDFT,
        SHOW_MODE_NB
    } show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;
    int xpos;
    double last_vis_time;
    RenderParams render_params;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    /* Maximum duration of a frame, above which we consider the jump a timestamp discontinuity. */
    double max_frame_duration;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    char *archive_path;
    char *entry_name;
    AVIOContext *archive_avio;
    char *ytdl_source_url;
    char *ytdl_audio_url;
    AVFormatContext *audio_ic;
    SDL_Thread *audio_read_tid;
    volatile int audio_seek_pending;
    int64_t audio_seek_pos;
    int64_t audio_seek_min;
    int64_t audio_seek_max;
    int audio_seek_flags;
    int ended_eof;
    int is_still_image;
    int width, height, xleft, ytop;
    int step;

    int vfilter_idx;
    AVFilterContext *in_video_filter; /* The first filter in the video chain. */
    AVFilterContext *out_video_filter; /* The last filter in the video chain. */
    AVFilterContext *in_audio_filter; /* The first filter in the audio chain. */
    AVFilterContext *out_audio_filter; /* The last filter in the audio chain. */
    AVFilterGraph *agraph;

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_Condition *continue_read_thread;
} VideoState;

/* Options specified by the user. */
static const AVInputFormat *file_iformat;
static const char *input_filename;

static PlaylistEntry *playlist_entries = NULL;
static int playlist_size = 0;
static int playlist_pos = 0;
static char **pending_dirs = NULL;
static int n_pending_dirs = 0;
static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static float display_scale = 1.0f;
static int cmd_width = 0;
static int cmd_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int benchmark;
static int borderless;
static int alwaysontop;
static int startup_volume = 100;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static const char **vfilters_list = NULL;
static int nb_vfilters = 0;
static char *afilters = NULL;
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;
static int enable_vulkan = 1;
static int disable_vulkan = 0;
static char *vulkan_params = NULL;
static char *vulkan_swap_mode = NULL;
static char *video_background = NULL;
static const char *hwaccel = NULL;
static const char *active_hwaccel = NULL;
static int no_hwaccel = 0;
static int fatal_error_pending = 0;
static int video_unscaled = 1;
static int enable_360sbs = 0;
static float sbs360_yaw = 0.0f;
static float sbs360_pitch = 0.0f;
static float sbs360_hfov = 90.0f;
static int sbs360_drag = 0;
static int sbs360_drag_last_x = 0;
static int sbs360_drag_last_y = 0;
static int is_fullscreen = 1;
static int start_windowed = 0;
static float autofit_larger = 0.85f;
static int64_t audio_callback_time;
static int global_muted = 0;
static int ytdl_disable = 0;
static const char *ytdl_path = NULL;
static const char *ytdl_format = NULL;

#define FF_QUIT_EVENT (SDL_EVENT_USER + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;

static const SDL_PixelFormat *renderer_texture_formats = NULL;
static SDL_AudioDeviceID audio_dev;
static SDL_AudioStream *audio_stream_dev;

static VkRenderer *vk_renderer;

#define OSD_STATUS_DURATION_MS 1000
#define OSD_SEEK_DURATION_MS 1000

static TTF_Font *osd_font = NULL;
static TTF_Font *osd_sym_font = NULL;
static SDL_Surface *osd_surface = NULL;
static SDL_Renderer *osd_sw_renderer = NULL;
static int64_t osd_status_show_until = 0;
static int64_t osd_seek_show_until = 0;
static int64_t osd_volume_show_until = 0;

/* Forward declaration for the OSD. */
static double get_master_clock(VideoState *is);

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
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

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg) {
    int ret = GROW_ARRAY(vfilters_list, nb_vfilters);
    if (ret < 0) {
        return ret;
    }

    vfilters_list[nb_vfilters - 1] = av_strdup(arg);
    if (!vfilters_list[nb_vfilters - 1]) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                                 enum AVSampleFormat fmt2, int64_t channel_count2) {
    /* If channel count == 1, planar and non-planar formats are the same. */
    if (channel_count1 == 1 && channel_count2 == 1) {
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    } else {
        return channel_count1 != channel_count2 || fmt1 != fmt2;
    }
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

    /* XXX */
    SDL_SignalCondition(q->cond);

    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
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

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index) {
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

static void packet_queue_flush(PacketQueue *q) {
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

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_Condition *empty_queue_cond) {
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

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) {
                    return -1;
                }

                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
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

static void decoder_destroy(Decoder *d) {
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

static Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f) {
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

static Frame *frame_queue_peek_readable(FrameQueue *f) {
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

static void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size) {
        f->windex = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_SignalCondition(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f) {
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

static int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

static int64_t frame_queue_last_pos(FrameQueue *f) {
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial) {
        return fp->pos;
    } else {
        return -1;
    }
}

static void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

static inline void fill_rectangle(int x, int y, int w, int h) {
    SDL_FRect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h) {
        SDL_RenderFillRect(renderer, &rect);
    }
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture) {
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

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    if (video_unscaled) {
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
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX((int)width, 1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    int i;
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
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0) {
        return -1;
    }
    switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                       frame->data[1], frame->linesize[1],
                                       frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
        } else {
            return -1;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
        } else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }

    return ret ? 0 : -1;
}

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 11, 100)
static enum AVAlphaMode sdl_supported_alpha_modes[] = {
    AVALPHA_MODE_UNSPECIFIED,
    AVALPHA_MODE_STRAIGHT,
};
#endif

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
            vk_renderer_update_360sbs(vk_renderer, sbs360_yaw, sbs360_pitch, sbs360_hfov);
        }
        vk_renderer_display(vk_renderer, vp->frame, &is->render_params);
        return;
    }

    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    int i;
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
    SDL_RenderTextureRotated(renderer, is->vid_texture, NULL, &dst_rectf, 0, NULL,
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

static inline int compute_mod(int a, int b) {
    return a < 0 ? a % b + b : a % b;
}

static void video_audio_display(VideoState *s) {
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    channels = s->audio_tgt.ch_layout.nb_channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used = s->show_mode == SHOW_MODE_WAVES ? s->width : (2 * nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used) {
            delay = data_used;
        }

        i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    if (s->show_mode == SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        h = s->height / nb_display_channels;
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2);
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE) {
                    i -= SAMPLE_ARRAY_SIZE;
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(s->xleft, y, s->width, 1);
        }
    } else {
        int err = 0;
        if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0) {
            return;
        }

        if (s->xpos >= s->width) {
            s->xpos = 0;
        }
        nb_display_channels = FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            const float rdft_scale = 1.0;
            av_tx_uninit(&s->rdft);
            av_freep(&s->real_data);
            av_freep(&s->rdft_data);
            s->rdft_bits = rdft_bits;
            s->real_data = av_malloc_array(nb_freq, 4 * sizeof(*s->real_data));
            s->rdft_data = av_malloc_array(nb_freq + 1, 2 * sizeof(*s->rdft_data));
            err = av_tx_init(&s->rdft, &s->rdft_fn, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !s->rdft_data) {
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            float *data_in[2];
            AVComplexFloat *data[2];
            SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data_in[ch] = s->real_data + 2 * nb_freq * ch;
                data[ch] = s->rdft_data + nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x - nb_freq) * (1.0 / nb_freq);
                    data_in[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE) {
                        i -= SAMPLE_ARRAY_SIZE;
                    }
                }
                s->rdft_fn(s->rdft, data[ch], data_in[ch], sizeof(float));
                data[ch][0].im = data[ch][nb_freq].re;
                data[ch][nb_freq].re = 0;
            }
            if (SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
                    int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                                       : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            SDL_RenderTexture(renderer, s->vis_texture, NULL, NULL);
        }
        if (!s->paused) {
            s->xpos++;
        }
    }
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return;
    }
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_DestroyAudioStream(audio_stream_dev);
        audio_stream_dev = NULL;
        audio_dev = 0;
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
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
    }

    avformat_close_input(&is->ic);
    avformat_close_input(&is->audio_ic);
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
    if (is->vis_texture) {
        SDL_DestroyTexture(is->vis_texture);
    }
    if (is->vid_texture) {
        SDL_DestroyTexture(is->vid_texture);
    }
    if (is->sub_texture) {
        SDL_DestroyTexture(is->sub_texture);
    }
    av_free(is);
}

static void do_exit(VideoState *is) {
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
    if (osd_sw_renderer) {
        SDL_DestroyRenderer(osd_sw_renderer);
        osd_sw_renderer = NULL;
    }
    SDL_DestroySurface(osd_surface);
    osd_surface = NULL;
    if (osd_font) {
        TTF_CloseFont(osd_font);
        osd_font = NULL;
    }
    if (osd_sym_font) {
        TTF_CloseFont(osd_sym_font);
        osd_sym_font = NULL;
    }
    TTF_Quit();
    SDL_Quit();
    exit(0);
}

static void sigterm_handler(int sig) {
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar) {
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

static int video_open(VideoState *is) {
    int w, h;

    w = cmd_width ? cmd_width : default_width;
    h = cmd_height ? cmd_height : default_height;

    if (!window_title) {
        window_title = input_filename;
    }

    SDL_SetWindowTitle(window, window_title);
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    SDL_SetWindowFullscreen(window, is_fullscreen);
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);

    is->width = w;
    is->height = h;

    /* Make sure the damn thing is centered. */
    if (is_fullscreen && screen_width && screen_height) {
        is->width = screen_width;
        is->height = screen_height;
    }

    return 0;
}

static void format_time(char *buf, int bufsz, double secs) {
    int s = (int)secs;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sc = s % 60;
    snprintf(buf, bufsz, "%02d:%02d:%02d", h, m, sc);
}

static void osd_text(SDL_Renderer *r, const char *text, int x, int y, SDL_Color fg) {
    if (!osd_font) {
        return;
    }
    SDL_Surface *surf = TTF_RenderText_Blended(osd_font, text, 0, fg);
    if (!surf) {
        return;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_FRect bg = {x - 2, y - 1, surf->w + 4, surf->h + 2};
    SDL_RenderFillRect(r, &bg);
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_FRect dst = {x, y, surf->w, surf->h};
        SDL_RenderTexture(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
}

static void osd_draw_to(SDL_Renderer *r, VideoState *is) {
    int64_t now = (int64_t)SDL_GetTicks();
    int show_status = (now < osd_status_show_until);
    int show_seek = (now < osd_seek_show_until);
    int show_volume = (now < osd_volume_show_until);

    SDL_Color fg = {255, 255, 255, 255};
    SDL_Color bar_bg = {80, 80, 80, 200};
    SDL_Color bar_fg = {210, 210, 210, 255};

    if (show_status) {
        double pos = get_master_clock(is);
        double dur_secs = (is->ic && is->ic->duration != AV_NOPTS_VALUE)
            ? (double)is->ic->duration / AV_TIME_BASE
            : 0.0;

        char pos_str[16], dur_str[16], line[64];
        format_time(pos_str, sizeof(pos_str), isnan(pos) || pos < 0 ? 0.0 : pos);
        format_time(dur_str, sizeof(dur_str), dur_secs);

        int pct = (dur_secs > 0 && !isnan(pos) && pos >= 0)
            ? (int)(100.0 * pos / dur_secs + 0.5)
            : 0;

        const char *state_str = is->paused ? " || " : " |> ";
        snprintf(line, sizeof(line), "%s  %s / %s  (%d%%)", state_str, pos_str, dur_str, pct);
        osd_text(r, line, 8, 8, fg);
    }

    if (show_seek && is->ic && is->ic->duration != AV_NOPTS_VALUE) {
        double pos = get_master_clock(is);
        double dur_secs = (double)is->ic->duration / AV_TIME_BASE;
        double frac = (dur_secs > 0 && !isnan(pos) && pos >= 0)
            ? FFMIN(pos / dur_secs, 1.0)
            : 0.0;

        int bar_w = is->width * 3 / 5;
        int bar_h = 6;
        int bar_x = (is->width - bar_w) / 2;
        int bar_y = is->height - is->height / 8;

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
        SDL_FRect bg_rect = {bar_x, bar_y, bar_w, bar_h};
        SDL_RenderFillRect(r, &bg_rect);

        int filled = (int)(bar_w * frac);
        if (filled > 0) {
            SDL_SetRenderDrawColor(r, bar_fg.r, bar_fg.g, bar_fg.b, bar_fg.a);
            SDL_FRect fg_rect = {bar_x, bar_y, filled, bar_h};
            SDL_RenderFillRect(r, &fg_rect);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

        double pos_clamped = (!isnan(pos) && pos >= 0) ? pos : 0.0;
        char pos_str[16], dur_str[16];
        format_time(pos_str, sizeof(pos_str), pos_clamped);
        format_time(dur_str, sizeof(dur_str), dur_secs);
        int tw, th;
        TTF_GetStringSize(osd_font, pos_str, 0, &tw, &th);
        osd_text(r, pos_str, bar_x, bar_y - th - 4, fg);
        TTF_GetStringSize(osd_font, dur_str, 0, &tw, &th);
        osd_text(r, dur_str, bar_x + bar_w - tw, bar_y - th - 4, fg);
    }

    if (show_volume) {
        int vol_pct = (int)(100.0 * is->audio_volume / FFP_MIX_MAXVOLUME + 0.5);
        int bar_w = is->width / 4;
        int bar_h = 8;
        int bar_x = (is->width - bar_w) / 2;
        int bar_y = is->height * 3 / 4;

        SDL_Color bar_bg = {80, 80, 80, 200};
        SDL_Color bar_fg = {210, 210, 210, 255};
        SDL_Color muted_fg = {210, 210, 210, 255};

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
        SDL_FRect bg_rect = {bar_x - 4, bar_y - bar_h / 2 - 4, bar_w + 8, bar_h + 8};
        SDL_RenderFillRect(r, &bg_rect);

        if (vol_pct > 0 && !is->muted) {
            int filled = (int)(bar_w * vol_pct / 100.0 + 0.5);
            SDL_SetRenderDrawColor(r, bar_fg.r, bar_fg.g, bar_fg.b, bar_fg.a);
            SDL_FRect fg_rect = {bar_x, bar_y - bar_h / 2, filled, bar_h};
            SDL_RenderFillRect(r, &fg_rect);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

        char vol_label[32];
        if (is->muted) {
            snprintf(vol_label, sizeof(vol_label), "%d%% ", vol_pct);
        } else {
            snprintf(vol_label, sizeof(vol_label), "%d%% ", vol_pct);
        }
        SDL_Color label_col = is->muted ? muted_fg : fg;

        if (osd_font) {
            int lw, lh;
            TTF_GetStringSize(osd_font, vol_label, 0, &lw, &lh);

            SDL_Surface *sym_surf = NULL;
            int sym_w = 0, sym_h = 0;
            if (osd_sym_font) {
                const char *sym;
                if (is->muted) {
                    sym = "\xEE\x84\x8A";
                } else if (vol_pct <= 33) {
                    sym = "\xEE\x84\x8B";
                } else if (vol_pct <= 66) {
                    sym = "\xEE\x84\x8C";
                } else {
                    sym = "\xEE\x84\x8D";
                }
                sym_surf = TTF_RenderText_Blended(osd_sym_font, sym, 0, label_col);
                if (sym_surf) {
                    sym_w = sym_surf->w;
                    sym_h = sym_surf->h;
                }
            }
            int gap = sym_surf ? 4 : 0;
            int row_h = sym_surf ? FFMAX(sym_h, lh) : lh;
            int row_y = bar_y - row_h - 8;
            int total_w = sym_w + gap + lw;
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
            SDL_FRect unified_bg = {bar_x - 2, row_y - 1, total_w + 4, row_h + 2};
            SDL_RenderFillRect(r, &unified_bg);

            if (sym_surf) {
                SDL_Texture *sym_tex = SDL_CreateTextureFromSurface(r, sym_surf);
                if (sym_tex) {
                    int sym_y = row_y + (row_h - sym_h) / 2;
                    SDL_FRect sym_dst = {bar_x, sym_y, sym_w, sym_h};
                    SDL_RenderTexture(r, sym_tex, NULL, &sym_dst);
                    SDL_DestroyTexture(sym_tex);
                }
                SDL_DestroySurface(sym_surf);
            }

            SDL_Surface *txt_surf = TTF_RenderText_Blended(osd_font, vol_label, 0, label_col);
            if (txt_surf) {
                SDL_Texture *txt_tex = SDL_CreateTextureFromSurface(r, txt_surf);
                if (txt_tex) {
                    int txt_y = row_y + (row_h - lh) / 2;
                    SDL_FRect txt_dst = {bar_x + sym_w + gap, txt_y, txt_surf->w, txt_surf->h};
                    SDL_RenderTexture(r, txt_tex, NULL, &txt_dst);
                    SDL_DestroyTexture(txt_tex);
                }
                SDL_DestroySurface(txt_surf);
            }
        }
    }
}

/* Vulkan path: render OSD into a software surface and store pixels in render_params. */
static void osd_prepare_vulkan(VideoState *is) {
    if (!osd_surface || osd_surface->w != is->width || osd_surface->h != is->height) {
        if (osd_sw_renderer) {
            SDL_DestroyRenderer(osd_sw_renderer);
            osd_sw_renderer = NULL;
        }
        SDL_DestroySurface(osd_surface);
        osd_surface = SDL_CreateSurface(is->width, is->height,
                                        SDL_PIXELFORMAT_ABGR8888);
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

    osd_draw_to(osd_sw_renderer, is);

    is->render_params.osd_pixels = osd_surface->pixels;
    is->render_params.osd_width = osd_surface->w;
    is->render_params.osd_height = osd_surface->h;
    is->render_params.osd_stride = osd_surface->pitch;
}

/* SDL renderer path: draw OSD directly into the hardware renderer. */
static void osd_draw(VideoState *is) {
    if (!renderer || !osd_font) {
        return;
    }
    int64_t now = (int64_t)SDL_GetTicks();
    if (now >= osd_status_show_until && now >= osd_seek_show_until && now >= osd_volume_show_until) {
        return;
    }
    osd_draw_to(renderer, is);
}

static void video_display(VideoState *is) {
    if (!is->width) {
        video_open(is);
    }

    is->render_params.osd_pixels = NULL;
    if (vk_renderer && osd_font) {
        int64_t now = (int64_t)SDL_GetTicks();
        if (now < osd_status_show_until || now < osd_seek_show_until || now < osd_volume_show_until) {
            osd_prepare_vulkan(is);
        }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO) {
        video_audio_display(is);
    } else if (is->video_st) {
        video_image_display(is);
    }
    osd_draw(is);
    SDL_RenderPresent(renderer);
}

static double get_clock(Clock *c) {
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

static void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial) {
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

static void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD)) {
        set_clock(c, slave_clock, slave->serial);
    }
}

static int get_master_sync_type(VideoState *is) {
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

static double get_master_clock(VideoState *is) {
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

static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes) {
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

static void toggle_pause(VideoState *is) {
    stream_toggle_pause(is);
    is->step = 0;
    osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
}

static void toggle_mute(VideoState *is) {
    is->muted = !is->muted;
    global_muted = is->muted;
    osd_volume_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
}

static void update_volume(VideoState *is, int sign, double step) {
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)FFP_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(FFP_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, FFP_MIX_MAXVOLUME);
    osd_volume_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
    is->force_refresh = 1;
}

static void step_to_next_frame(VideoState *is) {
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

    if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            /* Nothing to do. */
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
            delay = compute_target_delay(last_duration, is);

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
                duration = vp_duration(is, vp, nextvp);
                if (!benchmark && !is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
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

                    if (sp->serial != is->subtitleq.serial || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000)))) {
                        if (sp->uploaded) {
                            int i;
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
        if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown) {
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

static int get_video_frame(VideoState *is, AVFrame *frame) {
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0) {
        return -1;
    }

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret, i;
    int nb_filters = graph->nb_filters;
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

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame) {
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    const AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; renderer_texture_formats && renderer_texture_formats[i] != SDL_PIXELFORMAT_UNKNOWN; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if (renderer_texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }

    if (!av_dict_get(sws_dict, "sws_flags", NULL, 0)) {
        av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "flags=fast_bilinear:");
    }

    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
        }
    }
    if (strlen(sws_flags_str)) {
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';
    }

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "lachesis_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format = frame->format;
    par->time_base = is->video_st->time_base;
    par->width = frame->width;
    par->height = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space = frame->colorspace;
    par->color_range = frame->color_range;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(11, 8, 100)
    par->alpha_mode = frame->alpha_mode;
#endif
    par->frame_rate = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0) {
        goto fail;
    }

    ret = avfilter_init_dict(filt_src, NULL);
    if (ret < 0) {
        goto fail;
    }

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                           "lachesis_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (!vk_renderer &&
        (ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0) {
        goto fail;
    }
    if (!vk_renderer &&
        (ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_color_spaces),
                                AV_OPT_TYPE_INT, sdl_supported_color_spaces)) < 0) {
        goto fail;
    }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 11, 100)
    if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_alpha_modes),
                                AV_OPT_TYPE_INT, sdl_supported_alpha_modes)) < 0) {
        goto fail;
    }
#endif

    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0) {
        goto fail;
    }

    last_filter = filt_out;

    /* clang-format off */
/* XXX
 * This macro adds a filter before the last added filter, so the
 * processing order of the filters is in reverse.
 */
#define INSERT_FILT(name, arg)                                                 \
    do {                                                                       \
        AVFilterContext *filt_ctx;                                             \
                                                                               \
        ret = avfilter_graph_create_filter(&filt_ctx,                          \
                                           avfilter_get_by_name(name),         \
                                           "lachesis_" name, arg, NULL, graph); \
        if (ret < 0)                                                           \
            goto fail;                                                         \
                                                                               \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                      \
        if (ret < 0)                                                           \
            goto fail;                                                         \
                                                                               \
        last_filter = filt_ctx;                                                \
    } while (0)
    /* clang-format on */
    if (autorotate) {
        double theta = 0.0;
        int32_t *displaymatrix = NULL;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd) {
            displaymatrix = (int32_t *)sd->data;
        }
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd) {
                displaymatrix = (int32_t *)psd->data;
            }
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0) {
                INSERT_FILT("hflip", NULL);
            }
            if (displaymatrix[4] < 0) {
                INSERT_FILT("vflip", NULL);
            }
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0) {
                INSERT_FILT("vflip", NULL);
            }
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0) {
        goto fail;
    }

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);

    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format) {
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc())) {
        return AVERROR(ENOMEM);
    }
    is->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e))) {
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    }
    if (strlen(aresample_swr_opts)) {
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    }
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "lachesis_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0) {
        goto end;
    }

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "lachesis_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0) {
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 6, 100)
        static const enum AVSampleFormat sample_fmts[] = {
            AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
        ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,
                                  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
#endif
            goto end;
    }

    if (force_output_format) {
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 6, 100)
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0) {
            goto end;
        }
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0) {
            goto end;
        }
#else
        char layout[256];
        int sample_rates[] = {is->audio_tgt.freq, -1};
        av_channel_layout_describe(&is->audio_tgt.ch_layout, layout, sizeof(layout));
        if ((ret = av_opt_set(filt_asink, "ch_layouts", layout,
                              AV_OPT_SEARCH_CHILDREN)) < 0) {
            goto end;
        }
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates,
                                       -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            goto end;
        }
#endif
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0) {
        goto end;
    }

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0) {
        goto end;
    }

    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0) {
        avfilter_graph_free(&is->agraph);
    }
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int audio_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0) {
            goto the_end;
        }

        if (got_frame) {
            tb = (AVRational){1, frame->sample_rate};

            reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                               frame->format, frame->ch_layout.nb_channels) ||
                av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                is->audio_filter_src.freq != frame->sample_rate ||
                is->auddec.pkt_serial != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                is->audio_filter_src.fmt = frame->format;
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0) {
                    goto the_end;
                }
                is->audio_filter_src.freq = frame->sample_rate;
                last_serial = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, afilters, 1)) < 0) {
                    goto the_end;
                }
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0) {
                goto the_end;
            }

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq))) {
                    goto the_end;
                }

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audioq.serial != is->auddec.pkt_serial) {
                    break;
                }
            }
            if (ret == AVERROR_EOF) {
                is->auddec.finished = is->auddec.pkt_serial;
            }
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);

    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int video_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

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

        if (last_w != frame->width || last_h != frame->height || last_format != frame->format || last_serial != is->viddec.pkt_serial || last_vfilter_idx != is->vfilter_idx) {
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
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

static int subtitle_thread(void *arg) {
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

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE) {
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            }
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* Now we can update the picture count. */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }

    return 0;
}

/* XXX */
static void update_sample_display(VideoState *is, short *samples, int samples_size) {
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size) {
            len = size;
        }
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE) {
            is->sample_array_index = 0;
        }
        size -= len;
    }
}

static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
            }
        } else {
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused) {
        return -1;
    }

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2) {
                return -1;
            }
            av_usleep(1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq))) {
            return -1;
        }
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                                  &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                  &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                                  0, NULL);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0) {
            return -1;
        }
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) {
            return AVERROR(ENOMEM);
        }
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            return -1;
        }
        if (len2 == out_count) {
            if (swr_init(is->swr_ctx) < 0) {
                swr_free(&is->swr_ctx);
            }
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    if (!isnan(af->pts)) {
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    } else {
        is->audio_clock = NAN;
    }
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif

    return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);

static void SDLCALL sdl_audio_stream_callback(void *opaque, SDL_AudioStream *stream,
                                              int additional_amount, int total_amount) {
    if (additional_amount > 0) {
        Uint8 *data = av_malloc(additional_amount);
        if (data) {
            sdl_audio_callback(opaque, data, additional_amount);
            SDL_PutAudioStreamData(stream, data, additional_amount);
            av_free(data);
        }
    }
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* Just output silence upon error. */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != SHOW_MODE_VIDEO) {
                    update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
                }
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        if (!is->muted && is->audio_buf && is->audio_volume == FFP_MIX_MAXVOLUME) {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf) {
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, SDL_AUDIO_S16, len1, is->audio_volume / (float)FFP_MIX_MAXVOLUME);
            }
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec;
    const char *env;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;
    int buffer_frames;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        return -1;
    }
    wanted_spec.format = SDL_AUDIO_S16;

    audio_stream_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                 &wanted_spec,
                                                 sdl_audio_stream_callback, opaque);
    if (!audio_stream_dev) {
        return -1;
    }
    audio_dev = SDL_GetAudioStreamDevice(audio_stream_dev);

    {
        VideoState *is = opaque;
        SDL_AudioSpec dev_spec;
        int dev_frames = 0;
        const char *drv = SDL_GetCurrentAudioDriver();
        fprintf(stderr, "INFO: SDL audio device driver: %s, requested S16 %dch @ %dHz\n",
                drv ? drv : "(none)", wanted_spec.channels, wanted_spec.freq);
        if (SDL_GetAudioDeviceFormat(audio_dev, &dev_spec, &dev_frames)) {
            fprintf(stderr,
                    "INFO: SDL audio device format: fmt=0x%x %dch @ %dHz, buffer=%d frames\n",
                    (unsigned)dev_spec.format, dev_spec.channels, dev_spec.freq,
                    dev_frames);
        } else {
            /* XXX */
            is->av_sync_type = AV_SYNC_VIDEO_MASTER;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = wanted_spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0) {
        return -1;
    }
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        return -1;
    }

    buffer_frames = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                          2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    return buffer_frames * audio_hw_params->frame_size;
}

static int try_hwaccel(AVBufferRef **device_ctx, const char *name) {
    enum AVHWDeviceType type;
    int ret;
    AVBufferRef *vk_dev;

    type = av_hwdevice_find_type_by_name(name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return AVERROR(ENOTSUP);
    }

    if (vk_renderer) {
        ret = vk_renderer_get_hw_dev(vk_renderer, &vk_dev);
        if (ret >= 0) {
            ret = av_hwdevice_ctx_create_derived(device_ctx, type, vk_dev, 0);
            if (!ret) {
                return 0;
            }
            if (ret != AVERROR(ENOSYS)) {
                return ret;
            }
        }
    }

    return av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0);
}

static int create_hwaccel(AVBufferRef **device_ctx) {
    static const char *auto_hwaccels[] = {"vulkan", "vaapi", "videotoolbox", "d3d11va", NULL};
    int ret;

    *device_ctx = NULL;

    if (no_hwaccel) {
        return 0;
    }

    if (!vk_renderer) {
        if (hwaccel) {
            fprintf(stderr, "-hwaccel %s ignored because it requires the Vulkan renderer.\n", hwaccel);
        }
        return 0;
    }

    if (hwaccel) {
        ret = try_hwaccel(device_ctx, hwaccel);
        if (ret < 0 && ret != AVERROR(ENOSYS)) {
            fprintf(stderr, "hwaccel %s is not available!\n",
                    hwaccel);
        }
        if (ret >= 0) {
            active_hwaccel = hwaccel;
        }
        return ret < 0 ? ret : 0;
    }

    for (int i = 0; auto_hwaccels[i]; i++) {
        ret = try_hwaccel(device_ctx, auto_hwaccels[i]);
        if (!ret) {
            active_hwaccel = auto_hwaccels[i];
            return 0;
        }
        *device_ctx = NULL;
    }

    return 0;
}

static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = {0};
    int ret = 0;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->last_audio_stream = stream_index;
        forced_codec_name = audio_codec_name;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->last_subtitle_stream = stream_index;
        forced_codec_name = subtitle_codec_name;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->last_video_stream = stream_index;
        forced_codec_name = video_codec_name;
        break;
    default:
        break;
    }
    if (forced_codec_name) {
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    }
    if (!codec) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;

    if (fast) {
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    ret = filter_codec_opts(codec_opts, avctx->codec_id, ic,
                            ic->streams[stream_index], codec, &opts, NULL);
    if (ret < 0) {
        goto fail;
    }

    /* XXX */
    if (vk_renderer && !no_hwaccel && avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(62, 11, 100)
        av_dict_set(&opts, "threads", "1", 0);
#else
        av_dict_set(&opts, "threads", "auto", 0);
#endif
    }

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(&avctx->hw_device_ctx);
        if (ret < 0) {
            goto fail;
        }
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    ret = check_avoptions(opts);
    if (ret < 0) {
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
        AVFilterContext *sink;

        is->audio_filter_src.freq = avctx->sample_rate;
        ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
        if (ret < 0) {
            goto fail;
        }
        is->audio_filter_src.fmt = avctx->sample_fmt;
        if ((ret = configure_audio_filters(is, afilters, 0)) < 0) {
            goto fail;
        }
        sink = is->out_audio_filter;
        sample_rate = av_buffersink_get_sample_rate(sink);
        ret = av_buffersink_get_ch_layout(sink, &ch_layout);
        if (ret < 0) {
            goto fail;
        }
    }

        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0) {
            goto fail;
        }
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;

        is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0) {
            goto out;
        }
        SDL_ResumeAudioDevice(audio_dev);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0) {
            goto out;
        }
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0) {
            goto out;
        }
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
        queue->abort_request ||
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        ((queue->nb_packets > MIN_FRAMES && (!queue->duration)) || (av_q2d(st->time_base) * queue->duration > 1.0));
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }

    return 0;
}

static int detect_still_image(const AVFormatContext *ic) {
    if (!ic || !ic->iformat) {
        return 0;
    }
    const char *name = ic->iformat->name;
    static const char *const still_formats[] = {
        "image2",
        "png_pipe", "jpeg_pipe", "mjpeg_pipe", "bmp_pipe",
        "tiff_pipe", "tga_pipe", "dpx_pipe", "exr_pipe",
        "pnm_pipe", "sgi_pipe", "xwd_pipe", "xbm_pipe",
        NULL};
    for (int i = 0; still_formats[i]; i++) {
        if (!strcmp(name, still_formats[i])) {
            return 1;
        }
    }
    /* There might be an additional edge case to consider here. */
    if (!strcmp(name, "mjpeg")) {
        for (unsigned int i = 0; i < ic->nb_streams; i++) {
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                return 0;
            }
        }
        return 1;
    }
    if (!strcmp(name, "gif") || !strcmp(name, "webp_pipe")) {
        for (unsigned int i = 0; i < ic->nb_streams; i++) {
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return ic->streams[i]->nb_frames == 1;
            }
        }
    }

    return 0;
}

static const AVInputFormat *guess_archive_entry_format(const char *entry_name) {
    const char *dot = strrchr(entry_name, '.');
    if (!dot) {
        return NULL;
    }
    const char *ext = dot + 1;
    struct {
        const char *ext;
        const char *fmt;
    } map[] = {
        {"jpg", "mjpeg"},
        {"jpeg", "mjpeg"},
        {"png", "png_pipe"},
        {"bmp", "bmp_pipe"},
        {"gif", "gif"},
        {"webp", "webp_pipe"},
        {"tiff", "tiff_pipe"},
        {"tif", "tiff_pipe"},
        {"tga", "tga_pipe"},
        {NULL, NULL},
    };
    for (int i = 0; map[i].ext; i++) {
        if (!strcasecmp(ext, map[i].ext)) {
            return av_find_input_format(map[i].fmt);
        }
    }

    return NULL;
}

static int ytdl_resolve(const char *url, char **video_url, char **audio_url) {
    *video_url = NULL;
    *audio_url = NULL;
    const char *path = ytdl_path ? ytdl_path : "yt-dlp";
    const char *fmt = ytdl_format ? ytdl_format : "bestvideo+bestaudio/best";

#if defined(_WIN32)
    /* XXX: %VARS% */
    char *cmd = av_asprintf("\"\"%s\" -g --no-warnings --no-playlist -f \"%s\" -- \"%s\" 2>NUL\"",
                            path, fmt, url);
#else
    char *cmd = av_asprintf("%s -g --no-warnings --no-playlist -f '%s' -- '%s' 2>/dev/null",
                            path, fmt, url);
#endif
    if (!cmd) {
        return 0;
    }
#if defined(_WIN32)
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    av_free(cmd);
    if (!fp) {
        return 0;
    }

    int n = 0;
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!len) {
            continue;
        }
        if (n == 0) {
            *video_url = av_strdup(line);
        } else if (n == 1) {
            *audio_url = av_strdup(line);
        }
        n++;
    }
#if defined(_WIN32)
    _pclose(fp);
#else
    pclose(fp);
#endif

    if (!*video_url) {
        av_freep(audio_url);
        return 0;
    }
    return n;
}

static int audio_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request || is->audio_seek_pending;
}

static int audio_read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = is->audio_ic;
    AVPacket *pkt = av_packet_alloc();
    int sent_eof = 0;

    if (!pkt) {
        return AVERROR(ENOMEM);
    }

    for (;;) {
        if (is->abort_request) {
            break;
        }

        if (is->audio_seek_pending) {
            avformat_seek_file(ic, -1,
                               is->audio_seek_min,
                               is->audio_seek_pos,
                               is->audio_seek_max,
                               is->audio_seek_flags);
            is->audio_seek_pending = 0;
            sent_eof = 0;
            continue;
        }

        if (sent_eof) {
            SDL_Delay(50);
            continue;
        }

        if (!infinite_buffer &&
            stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq)) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                sent_eof = 1;
            } else if (!is->audio_seek_pending && !is->abort_request) {
                SDL_Delay(10);
            }
            continue;
        }

        if (pkt->stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);

    return 0;
}

static void print_stream_info(const VideoState *is);

/* This thread gets the stream from disk or the network. */
static int read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    const AVDictionaryEntry *t;
    SDL_Mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if (is->archive_path && is->entry_name) {
        is->archive_avio = archive_entry_open_avio(is->archive_path, is->entry_name);
        if (!is->archive_avio) {
            fprintf(stderr, "Could not open archive entry '%s' in '%s'!\n",
                    is->entry_name, is->archive_path);
            ret = -1;
            goto fail;
        }
        ic->pb = is->archive_avio;
        ic->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    if (!is->archive_path && !ytdl_disable) {
        const char *fn = is->filename;
        if (strncmp(fn, "http://", 7) == 0 || strncmp(fn, "https://", 8) == 0 ||
            strncmp(fn, "ytdl://", 7) == 0) {
            if (strncmp(fn, "ytdl://", 7) == 0) {
                char *bare = av_strdup(fn + 7);
                if (bare) {
                    av_free(is->filename);
                    is->filename = bare;
                    fn = is->filename;
                }
            }
            char *vurl = NULL, *aurl = NULL;
            if (ytdl_resolve(fn, &vurl, &aurl) > 0) {
                is->ytdl_source_url = av_strdup(fn);
                av_free(is->filename);
                is->filename = vurl;
                is->ytdl_audio_url = aurl;
                av_dict_set(&format_opts, "reconnect", "1", 0);
                av_dict_set(&format_opts, "reconnect_streamed", "1", 0);
                av_dict_set(&format_opts, "reconnect_on_network_error", "1", 0);
            } else {
                fprintf(stderr, "yt-dlp failed, so trying direct open.\n");
            }
        }
    }
    {
        const char *open_url = (is->archive_path && is->entry_name)
            ? is->entry_name
            : is->filename;
        const AVInputFormat *open_fmt = is->iformat;
        if (!open_fmt && is->archive_avio && is->entry_name) {
            open_fmt = guess_archive_entry_format(is->entry_name);
        }
        err = avformat_open_input(&ic, open_url, open_fmt, &format_opts);
    }
    if (scan_all_pmts_set) {
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    }
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    remove_avoptions(&format_opts, codec_opts);

    ret = check_avoptions(format_opts);
    if (ret < 0) {
        goto fail;
    }
    is->ic = ic;

    if (genpts) {
        ic->flags |= AVFMT_FLAG_GENPTS;
    }

    if (find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic->nb_streams;

        err = setup_find_stream_info_opts(ic, codec_opts, &opts);
        if (err < 0) {
            ret = err;
            goto fail;
        }

        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++) {
            av_dict_free(&opts[i]);
        }
        av_freep(&opts);

        if (err < 0) {
            if (!is->archive_avio) {
                ret = -1;
                goto fail;
            }
        }
    }

    is->is_still_image = detect_still_image(ic);

    if (ic->pb) {
        ic->pb->eof_reached = 0;
        if (is->archive_avio && is->is_still_image) {
            avio_seek(ic->pb, 0, SEEK_SET);
        }
    }

    if (seek_by_bytes < 0) {
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
            !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
            strcmp("ogg", ic->iformat->name);
    }

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0))) {
        window_title = av_asprintf("%s - %s", t->value, input_filename);
    }

    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        if (ic->start_time != AV_NOPTS_VALUE) {
            timestamp += ic->start_time;
        }
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    }

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1) {
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0) {
                st_index[type] = i;
            }
        }
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
    ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable) {
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    }
    if (!audio_disable) {
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    }
    if (!video_disable && !subtitle_disable) {
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);
    }

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width) {
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
        if (ret < 0 && hwaccel && !no_hwaccel) {
            fatal_error_pending = 1;
            goto fail;
        }
    }
    if (is->show_mode == SHOW_MODE_NONE) {
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    print_stream_info(is);

    if (is->ytdl_audio_url && !audio_disable && is->audio_stream < 0) {
        AVFormatContext *aic = avformat_alloc_context();
        if (aic) {
            aic->interrupt_callback.callback = audio_interrupt_cb;
            aic->interrupt_callback.opaque = is;
            AVDictionary *audio_opts = NULL;
            av_dict_set(&audio_opts, "reconnect", "1", 0);
            av_dict_set(&audio_opts, "reconnect_streamed", "1", 0);
            av_dict_set(&audio_opts, "reconnect_on_network_error", "1", 0);
            int audio_open_ret = avformat_open_input(&aic, is->ytdl_audio_url, NULL, &audio_opts);
            av_dict_free(&audio_opts);
            if (audio_open_ret >= 0 &&
                avformat_find_stream_info(aic, NULL) >= 0) {
                int aidx = av_find_best_stream(aic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
                if (aidx >= 0) {
                    is->audio_ic = aic;
                    AVFormatContext *save_ic = is->ic;
                    is->ic = aic;
                    stream_component_open(is, aidx);
                    is->ic = save_ic;
                    is->audio_read_tid = SDL_CreateThread(audio_read_thread,
                                                          "audio_read", is);
                    if (!is->audio_read_tid) {
                        avformat_close_input(&is->audio_ic);
                    }
                } else {
                    avformat_close_input(&aic);
                }
            } else {
                avformat_close_input(&aic);
            }
        }
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime) {
        infinite_buffer = 1;
    }

    for (;;) {
        if (is->abort_request) {
            break;
        }
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) {
                is->read_pause_return = av_read_pause(ic);
            } else {
                av_read_play(ic);
            }
        }
#if LACHESIS_HAVE_RTSP_DEMUXER || LACHESIS_HAVE_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* XXX */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            /* XXX */
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
                if (is->audio_ic) {
                    is->audio_seek_min = seek_min;
                    is->audio_seek_pos = seek_target;
                    is->audio_seek_max = seek_max;
                    is->audio_seek_flags = is->seek_flags;
                    is->audio_seek_pending = 1;
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused) {
                step_to_next_frame(is);
            }
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0) {
                    goto fail;
                }
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        if (infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) && stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) && stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            SDL_LockMutex(wait_mutex);
            SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (is->is_still_image) {
                is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = 1;
            } else if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (is->ytdl_source_url) {
                SDL_LockMutex(wait_mutex);
                SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 100);
                SDL_UnlockMutex(wait_mutex);
                continue;
            } else {
                /* Signal a clean EOF so the event loop can auto-advance or exit. */
                is->ended_eof = 1;
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0) {
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                }
                if (is->audio_stream >= 0) {
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                }
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                if (autoexit) {
                    goto fail;
                } else {
                    break;
                }
            }
            SDL_LockMutex(wait_mutex);
            SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }

        ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        ic->streams[pkt->stream_index]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(ic->streams[pkt->stream_index]->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000 <=
                ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range && !is->audio_ic) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
        /* ic->pb (archive_avio) is not freed by avformat_close_input with
         * AVFMT_FLAG_CUSTOM_IO, so free it now since is->ic was never set.
         */
        archive_entry_close_avio(is->archive_avio);
        is->archive_avio = NULL;
    }
    /* If is->ic was set, stream_close() will call avformat_close_input and
     * then archive_entry_close_avio via is->archive_avio.
     */
    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);

    return 0;
}

static VideoState *stream_open(const char *filename,
                               const AVInputFormat *iformat,
                               const char *archive_path,
                               const char *entry_name) {
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is) {
        return NULL;
    }
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
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
    int vol = av_clip(startup_volume, 0, 100);
    vol = av_clip(FFP_MIX_MAXVOLUME * vol / 100, 0, FFP_MIX_MAXVOLUME);
    is->audio_volume = vol;
    is->muted = global_muted;
    is->av_sync_type = av_sync_type;
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

static void stream_cycle_channel(VideoState *is, int codec_type) {
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
                if (p->stream_index[start_index] == stream_index) {
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
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
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

static void toggle_fullscreen(VideoState *is) {
    is_fullscreen = !is_fullscreen;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        SDL_SetWindowSize(window, default_width, default_height);
        is->width = default_width;
        is->height = default_height;
        is->force_refresh = 1;
    }
}

static void toggle_audio_display(VideoState *is) {
    int next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while ((((next != is->show_mode) && (next == SHOW_MODE_VIDEO && !is->video_st)) || (next != SHOW_MODE_VIDEO && !is->audio_st)));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = next;
    }
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_HideCursor();
            cursor_hidden = 1;
        }
        if (!benchmark && remaining_time > 0.0) {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh)) {
            video_refresh(is, &remaining_time);
        }
        {
            int64_t now_ms = (int64_t)SDL_GetTicks();
            int64_t osd_until = FFMAX(osd_status_show_until,
                                      FFMAX(osd_seek_show_until, osd_volume_show_until));
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
                osd_status_show_until = 0;
                osd_seek_show_until = 0;
                osd_volume_show_until = 0;
            }
        }
        SDL_PumpEvents();
    }
}

static void seek_chapter(VideoState *is, int incr) {
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters) {
        return;
    }

    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters) {
        return;
    }

    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base, AV_TIME_BASE_Q), 0, 0);
}

static void print_current_file(const VideoState *is) {
    if (is) {
        const char *url = is->ytdl_source_url ? is->ytdl_source_url : is->filename;
        if (url) {
            fprintf(stderr, "INFO: %s\n", url);
        }
    }
}

static void print_stream_info(const VideoState *is) {
    if (!is) {
        return;
    }

    fprintf(stderr, "INFO: Using hwaccel: %s\n",
            active_hwaccel ? active_hwaccel : "none (software decoding)");

    if (is->video_st) {
        AVCodecParameters *par = is->video_st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(is->ic, is->video_st, NULL);
        AVRational fr = av_guess_frame_rate(is->ic, (AVStream *)is->video_st, NULL);
        AVRational dar = {0, 1};
        char dar_buf[32];

        if (sar.num && sar.den) {
            av_reduce(&dar.num, &dar.den,
                      par->width * (int64_t)sar.num,
                      par->height * (int64_t)sar.den, 1024 * 1024);
            snprintf(dar_buf, sizeof(dar_buf), "%d:%d", dar.num, dar.den);
        } else {
            snprintf(dar_buf, sizeof(dar_buf), "N/A");
        }

        fprintf(stderr, "INFO: Video: %s, %dx%d, SAR %d:%d DAR %s",
                avcodec_get_name(par->codec_id), par->width, par->height,
                sar.num ? sar.num : 0, sar.den ? sar.den : 1, dar_buf);
        if (fr.num && fr.den) {
            fprintf(stderr, ", %.4g fps", av_q2d(fr));
        }
        fprintf(stderr, "\n");
    }

    if (is->audio_st) {
        AVCodecParameters *par = is->audio_st->codecpar;
        char chl[64];

        av_channel_layout_describe(&par->ch_layout, chl, sizeof(chl));
        fprintf(stderr, "INFO: Audio: %s, %d Hz, %s\n",
                avcodec_get_name(par->codec_id), par->sample_rate, chl);
    }
}

static void playlist_switch(VideoState **pis, int new_pos) {
    if (new_pos < 0 || new_pos >= playlist_size) {
        return;
    }
    stream_close(*pis);
    *pis = NULL;
    playlist_pos = new_pos;
    window_title = NULL;
    VideoState *is = stream_open_playlist_entry(playlist_pos);
    if (!is) {
        fprintf(stderr, "Failed to open playlist entry %d!\n", playlist_pos);
        do_exit(NULL);
    }
    print_current_file(is);
    *pis = is;
}

static void event_loop(VideoState **pis) {
    VideoState *cur_stream = *pis;
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        cur_stream = *pis;
        refresh_loop_wait_event(cur_stream, &event);
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (exit_on_keydown || event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                do_exit(cur_stream);
                break;
            }
            if (!cur_stream->width) {
                continue;
            }
            if ((event.key.mod & SDL_KMOD_ALT) && (event.key.mod & SDL_KMOD_SHIFT)) {
                SDL_Scancode sc = event.key.scancode;
                if (sc == SDL_SCANCODE_EQUALS) {
                    display_scale = FFMIN(display_scale + 0.1f, 4.0f);
                    cur_stream->force_refresh = 1;
                    break;
                } else if (sc == SDL_SCANCODE_MINUS) {
                    display_scale = FFMAX(display_scale - 0.1f, 0.1f);
                    cur_stream->force_refresh = 1;
                    break;
                }
            }
            switch (event.key.key) {
            case SDLK_F:
                toggle_fullscreen(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_P:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    int64_t now = (int64_t)SDL_GetTicks();
                    osd_status_show_until = now + OSD_STATUS_DURATION_MS;
                    if (cur_stream->ic && cur_stream->ic->duration != AV_NOPTS_VALUE) {
                        osd_seek_show_until = now + OSD_STATUS_DURATION_MS;
                    }
                    cur_stream->force_refresh = 1;
                    break;
                }
            case SDLK_SPACE:
                toggle_pause(cur_stream);
                osd_seek_show_until = (int64_t)SDL_GetTicks() + OSD_SEEK_DURATION_MS;
                osd_status_show_until = osd_seek_show_until;
                cur_stream->force_refresh = 1;
                break;
            case SDLK_M:
                toggle_mute(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
                cur_stream->force_refresh = 1;
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                osd_status_show_until = (int64_t)SDL_GetTicks() + OSD_STATUS_DURATION_MS;
                cur_stream->force_refresh = 1;
                break;
            case SDLK_S:
                step_to_next_frame(cur_stream);
                break;
            case SDLK_PERIOD:
            case SDLK_GREATER:
                if (playlist_size > 1) {
                    playlist_switch(pis, (playlist_pos + 1) % playlist_size);
                }
                break;
            case SDLK_COMMA:
            case SDLK_LESS:
                if (playlist_size > 1) {
                    playlist_switch(pis, (playlist_pos - 1 + playlist_size) % playlist_size);
                }
                break;
            case SDLK_A:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_V:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_C:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_T:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_W:
                if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters) {
                        cur_stream->vfilter_idx = 0;
                    }
                } else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
                break;
            case SDLK_PAGEUP:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                osd_seek_show_until = (int64_t)SDL_GetTicks() + OSD_SEEK_DURATION_MS;
                osd_status_show_until = osd_seek_show_until;
                if (seek_by_bytes) {
                    pos = -1;
                    if (pos < 0 && cur_stream->video_stream >= 0) {
                        pos = frame_queue_last_pos(&cur_stream->pictq);
                    }
                    if (pos < 0 && cur_stream->audio_stream >= 0) {
                        pos = frame_queue_last_pos(&cur_stream->sampq);
                    }
                    if (pos < 0) {
                        pos = avio_tell(cur_stream->ic->pb);
                    }
                    if (cur_stream->ic->bit_rate) {
                        incr *= cur_stream->ic->bit_rate / 8.0;
                    } else {
                        incr *= 180000.0;
                    }
                    pos += incr;
                    stream_seek(cur_stream, pos, incr, 1);
                } else {
                    pos = get_master_clock(cur_stream);
                    if (isnan(pos)) {
                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                    }
                    pos += incr;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE) {
                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                    }
                    stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                }
                break;
            case SDLK_I:
                if (enable_360sbs) {
                    sbs360_pitch = FFMIN(sbs360_pitch + 5.0f, 90.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_K:
                if (enable_360sbs) {
                    sbs360_pitch = FFMAX(sbs360_pitch - 5.0f, -90.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_J:
                if (enable_360sbs) {
                    sbs360_yaw -= 5.0f;
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_L:
                if (enable_360sbs) {
                    sbs360_yaw += 5.0f;
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS:
                if (enable_360sbs) {
                    sbs360_hfov = FFMAX(sbs360_hfov - 10.0f, 10.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                if (enable_360sbs) {
                    sbs360_hfov = FFMIN(sbs360_hfov + 10.0f, 170.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_KP_3:
                if (vk_renderer) {
                    enable_360sbs = !enable_360sbs;
                    vk_renderer_enable_360sbs(vk_renderer, enable_360sbs);
                    cur_stream->force_refresh = 1;
                }
                break;
            default:
                break;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (enable_360sbs && event.button.button == SDL_BUTTON_LEFT) {
                sbs360_drag = 0;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (enable_360sbs && event.button.button == SDL_BUTTON_LEFT) {
                sbs360_drag = 1;
                sbs360_drag_last_x = event.button.x;
                sbs360_drag_last_y = event.button.y;
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    toggle_fullscreen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_EVENT_MOUSE_MOTION:
            if (cursor_hidden) {
                SDL_ShowCursor();
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (enable_360sbs && sbs360_drag && event.type == SDL_EVENT_MOUSE_MOTION) {
                int dx = event.motion.x - sbs360_drag_last_x;
                int dy = event.motion.y - sbs360_drag_last_y;
                sbs360_drag_last_x = event.motion.x;
                sbs360_drag_last_y = event.motion.y;
                if (dx || dy) {
                    float deg_per_px = sbs360_hfov / (float)(cur_stream->width ? cur_stream->width : 1) * 1.0f;
                    sbs360_yaw += dx * deg_per_px;
                    sbs360_pitch = av_clipf(sbs360_pitch - dy * deg_per_px, -90.0f, 90.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT) {
                    break;
                }
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK)) {
                    break;
                }
                x = event.motion.x;
            }
            if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                uint64_t size = avio_size(cur_stream->ic->pb);
                stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
            } else {
                int64_t ts;
                frac = x / cur_stream->width;
                ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE) {
                    ts += cur_stream->ic->start_time;
                }
                stream_seek(cur_stream, ts, 0, 0);
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            screen_width = event.window.data1;
            screen_height = event.window.data2;
            if (is_fullscreen) {
                cur_stream->width = screen_width;
                cur_stream->height = screen_height;
            } else {
                cur_stream->width = default_width;
                cur_stream->height = default_height;
            }
            if (cur_stream->vis_texture) {
                SDL_DestroyTexture(cur_stream->vis_texture);
                cur_stream->vis_texture = NULL;
            }
            if (vk_renderer) {
                vk_renderer_resize(vk_renderer, screen_width, screen_height);
            }
        case SDL_EVENT_WINDOW_EXPOSED:
            cur_stream->force_refresh = 1;
            break;
        case SDL_EVENT_QUIT:
            do_exit(*pis);
            break;
        case FF_QUIT_EVENT: {
            VideoState *old = event.user.data1;
            if (old && old != *pis) {
                break;
            }
            if (fatal_error_pending) {
                do_exit(*pis);
            }
            int can_advance = playlist_pos + 1 < playlist_size;
            if (old && old->ended_eof) {
                if (can_advance) {
                    playlist_switch(pis, playlist_pos + 1);
                } else {
                    do_exit(*pis);
                }
            } else if (can_advance) {
                playlist_switch(pis, playlist_pos + 1);
            } else {
                do_exit(*pis);
            }
            break;
        }
        default:
            break;
        }
    }
}

static int opt_width(void *optctx, const char *opt, const char *arg) {
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0) {
        return ret;
    }
    cmd_width = num;

    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg) {
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0) {
        return ret;
    }
    cmd_height = num;

    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg) {
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        return AVERROR(EINVAL);
    }

    return 0;
}

static int opt_sync(void *optctx, const char *opt, const char *arg) {
    if (!strcmp(arg, "audio")) {
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    } else if (!strcmp(arg, "video")) {
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    } else if (!strcmp(arg, "ext")) {
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    } else {
        fatal_quit("Unknown value for %s: %s.\n", opt, arg);
    }

    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg) {
    show_mode = !strcmp(arg, "video") ? SHOW_MODE_VIDEO : !strcmp(arg, "waves") ? SHOW_MODE_WAVES
        : !strcmp(arg, "rdft")                                                  ? SHOW_MODE_RDFT
                                                                                : SHOW_MODE_NONE;

    if (show_mode == SHOW_MODE_NONE) {
        double num;
        int ret = parse_number(opt, arg, OPT_TYPE_INT, 0, SHOW_MODE_NB - 1, &num);
        if (ret < 0) {
            return ret;
        }
        show_mode = num;
    }

    return 0;
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

static int opt_input_file(void *optctx, const char *filename) {
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

static int opt_codec(void *optctx, const char *opt, const char *arg) {
    const char *spec = strchr(opt, ':');
    const char **name;
    if (!spec) {
        return AVERROR(EINVAL);
    }
    spec++;

    switch (spec[0]) {
    case 'a':
        name = &audio_codec_name;
        break;
    case 's':
        name = &subtitle_codec_name;
        break;
    case 'v':
        name = &video_codec_name;
        break;
    default:
        return AVERROR(EINVAL);
    }

    av_freep(name);
    *name = av_strdup(arg);

    return *name ? 0 : AVERROR(ENOMEM);
}

static int dummy;

static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS /* Just a comment to make clang-format ignore this line. */
    {"x", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
    {"y", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
    {"fs", OPT_TYPE_BOOL, 0, {&is_fullscreen}, "force fullscreen"},
    {"windowed", OPT_TYPE_BOOL, 0, {&start_windowed}, "start windowed instead of fullscreen"},
    {"autofit", OPT_TYPE_FLOAT, 0, {&autofit_larger}, "limit windowed size to this fraction of the display (default 0.85)", "fraction"},
    {"an", OPT_TYPE_BOOL, 0, {&audio_disable}, "disable audio"},
    {"vn", OPT_TYPE_BOOL, 0, {&video_disable}, "disable video"},
    {"sn", OPT_TYPE_BOOL, 0, {&subtitle_disable}, "disable subtitling"},
    {"ast", OPT_TYPE_STRING, OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]}, "select desired audio stream", "stream_specifier"},
    {"vst", OPT_TYPE_STRING, OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]}, "select desired video stream", "stream_specifier"},
    {"sst", OPT_TYPE_STRING, OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]}, "select desired subtitle stream", "stream_specifier"},
    {"ss", OPT_TYPE_TIME, 0, {&start_time}, "seek to a given position in seconds", "pos"},
    {"t", OPT_TYPE_TIME, 0, {&duration}, "play  \"duration\" seconds of audio/video", "duration"},
    {"bytes", OPT_TYPE_INT, 0, {&seek_by_bytes}, "seek by bytes 0=off 1=on -1=auto", "val"},
    {"seek_interval", OPT_TYPE_FLOAT, 0, {&seek_interval}, "set seek interval for left/right keys, in seconds", "seconds"},
    {"nodisp", OPT_TYPE_BOOL, 0, {&display_disable}, "disable graphical display"},
    {"benchmark", OPT_TYPE_BOOL, OPT_EXPERT, {&benchmark}, "blaze it (for benchmarking)", ""},
    {"noborder", OPT_TYPE_BOOL, 0, {&borderless}, "borderless window"},
    {"alwaysontop", OPT_TYPE_BOOL, 0, {&alwaysontop}, "window always on top"},
    {"volume", OPT_TYPE_INT, 0, {&startup_volume}, "set startup volume 0=min 100=max", "volume"},
    {"f", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_format}, "force format", "fmt"},
    {"fast", OPT_TYPE_BOOL, OPT_EXPERT, {&fast}, "non spec compliant optimizations", ""},
    {"genpts", OPT_TYPE_BOOL, OPT_EXPERT, {&genpts}, "generate pts", ""},
    {"drp", OPT_TYPE_INT, OPT_EXPERT, {&decoder_reorder_pts}, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    {"sync", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, {.func_arg = opt_sync}, "set audio-video sync. type (type=audio/video/ext)", "type"},
    {"autoexit", OPT_TYPE_BOOL, OPT_EXPERT, {&autoexit}, "exit at the end", ""},
    {"exitonkeydown", OPT_TYPE_BOOL, OPT_EXPERT, {&exit_on_keydown}, "exit on key down", ""},
    {"exitonmousedown", OPT_TYPE_BOOL, OPT_EXPERT, {&exit_on_mousedown}, "exit on mouse down", ""},
    {"loop", OPT_TYPE_INT, OPT_EXPERT, {&loop}, "set number of times the playback will be looped", "loop count"},
    {"framedrop", OPT_TYPE_BOOL, OPT_EXPERT, {&framedrop}, "drop frames when cpu is too slow", ""},
    {"infbuf", OPT_TYPE_BOOL, OPT_EXPERT, {&infinite_buffer}, "don't limit the input buffer size (useful with realtime streams)", ""},
    {"window_title", OPT_TYPE_STRING, 0, {&window_title}, "set window title", "window title"},
    {"left", OPT_TYPE_INT, OPT_EXPERT, {&screen_left}, "set the x position for the left of the window", "x pos"},
    {"top", OPT_TYPE_INT, OPT_EXPERT, {&screen_top}, "set the y position for the top of the window", "y pos"},
    {"vf", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
    {"af", OPT_TYPE_STRING, 0, {&afilters}, "set audio filters", "filter_graph"},
    {"rdftspeed", OPT_TYPE_INT, OPT_AUDIO | OPT_EXPERT, {&rdftspeed}, "rdft speed", "msecs"},
    {"showmode", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode"},
    {"i", OPT_TYPE_BOOL, 0, {&dummy}, "read specified file", "input_file"},
    {"codec", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_codec}, "force decoder", "decoder_name"},
    {"acodec", OPT_TYPE_STRING, OPT_EXPERT, {&audio_codec_name}, "force audio decoder", "decoder_name"},
    {"scodec", OPT_TYPE_STRING, OPT_EXPERT, {&subtitle_codec_name}, "force subtitle decoder", "decoder_name"},
    {"vcodec", OPT_TYPE_STRING, OPT_EXPERT, {&video_codec_name}, "force video decoder", "decoder_name"},
    {"autorotate", OPT_TYPE_BOOL, 0, {&autorotate}, "automatically rotate video", ""},
    {"find_stream_info", OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT, {&find_stream_info}, "read and decode the streams to fill missing information with heuristics"},
    {"filter_threads", OPT_TYPE_INT, OPT_EXPERT, {&filter_nbthreads}, "number of filter threads per graph"},
    {"enable_vulkan", OPT_TYPE_BOOL, 0, {&enable_vulkan}, "enable vulkan renderer"},
    {"no-vulkan", OPT_TYPE_BOOL, 0, {&disable_vulkan}, "disable the vulkan renderer"},
    {"vulkan_params", OPT_TYPE_STRING, OPT_EXPERT, {&vulkan_params}, "vulkan configuration using a list of key=value pairs separated by ':'"},
    {"vulkan-swap-mode", OPT_TYPE_STRING, OPT_EXPERT, {&vulkan_swap_mode}, "vulkan present mode: fifo, fifo-relaxed, mailbox or immediate", "mode"},
    {"video_bg", OPT_TYPE_STRING, OPT_EXPERT, {&video_background}, "set video background for transparent videos"},
    {"hwaccel", OPT_TYPE_STRING, OPT_EXPERT, {&hwaccel}, "use hardware accelerated decoding"},
    {"no-hwaccel", OPT_TYPE_BOOL, 0, {&no_hwaccel}, "disable hardware accelerated decoding (force software)"},
    {"video_unscaled", OPT_TYPE_BOOL, 0, {&video_unscaled}, "display video at native size, scale down only if too large"},
    {"360-sbs", OPT_TYPE_BOOL, 0, {&enable_360sbs}, "enable 360\xc2\xb0 equirectangular projection (requires -enable_vulkan)"},
    {"no-ytdl", OPT_TYPE_BOOL, 0, {&ytdl_disable}, "disable yt-dlp integration"},
    {"ytdl-path", OPT_TYPE_STRING, 0, {&ytdl_path}, "path to yt-dlp binary", "path"},
    {"ytdl-format", OPT_TYPE_STRING, 0, {&ytdl_format}, "yt-dlp format selection string", "format"},
    {
        NULL,
    },
};

void show_help_default(const char *opt, const char *arg) {
    show_help_options(options, 0, OPT_EXPERT);
    show_help_options(options, OPT_EXPERT, 0);
}

int main(int argc, char **argv) {
    int flags, ret;
    VideoState *is;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_ERROR);
    parse_loglevel(argc, argv, options);

#if LACHESIS_HAVE_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    ret = parse_options(NULL, argc, argv, options, opt_input_file);
    if (ret < 0) {
        exit(ret == AVERROR_EXIT ? 0 : 1);
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
        fatal_quit("An input file must be specified.\n");
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
    } else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size.
         */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE")) {
            SDL_setenv_unsafe("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
        }
    }
    if (display_disable) {
        flags &= ~SDL_INIT_VIDEO;
    }
    if (!SDL_Init(flags)) {
        fatal_quit("Could not initialize SDL: %s!\n", SDL_GetError());
    }

    SDL_SetEventEnabled(SDL_EVENT_USER, false);

    if (start_windowed) {
        is_fullscreen = 0;
    }

    if (disable_vulkan) {
        enable_vulkan = 0;
    }

    if (!display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (is_fullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        if (alwaysontop) {
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        }
        if (borderless) {
            flags |= SDL_WINDOW_BORDERLESS;
        } else {
            flags |= SDL_WINDOW_RESIZABLE;
        }

        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
        if (hwaccel && (!strcmp(hwaccel, "none") || !strcmp(hwaccel, "no") || !strcmp(hwaccel, "off") || !strcmp(hwaccel, "0"))) {
            no_hwaccel = 1;
            hwaccel = NULL;
        }
        if (hwaccel && !no_hwaccel && !enable_vulkan && !disable_vulkan) {
            enable_vulkan = 1;
        }
        if (enable_360sbs && !enable_vulkan) {
            fatal_quit("-360-sbs requires Vulkan.\n");
        }
        if (enable_vulkan) {
            vk_renderer = vk_get_renderer();
            if (vk_renderer) {
                flags |= SDL_WINDOW_VULKAN;
            } else {
                fprintf(stderr, "Your SDL version doesn't support Vulkan.\n");
                enable_vulkan = 0;
            }
        }
        window = SDL_CreateWindow(program_name, default_width, default_height, flags);
        if (!window) {
            fatal_quit("Failed to create window: %s!", SDL_GetError());
        }

        if (vk_renderer) {
            AVDictionary *dict = NULL;

            if (vulkan_params) {
                int ret = av_dict_parse_string(&dict, vulkan_params, "=", ":", 0);
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
            ret = vk_renderer_create(vk_renderer, window, dict);
            av_dict_free(&dict);
            if (ret < 0) {
                fatal_quit("Failed to create Vulkan renderer!\n");
            }
            if (enable_360sbs) {
                ret = vk_renderer_enable_360sbs(vk_renderer, 1);
                if (ret < 0) {
                    fatal_quit("Failed to enable 360 SBS shader!\n");
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
                fatal_quit("Failed to create window or renderer: %s!", SDL_GetError());
            }
        }

        if (TTF_Init()) {
            /* XXX */
            SDL_IOStream *rw = SDL_IOFromConstMem(osd_font_data, (size_t)osd_font_size);
            if (rw) {
                osd_sym_font = TTF_OpenFontIO(rw, true, 24.0f);
            }

#if !defined(_WIN32) && !defined(__APPLE__)
            {
                FILE *fp = popen("fc-match --format=%{file} monospace 2>/dev/null", "r");
                if (fp) {
                    char path[512] = {0};
                    if (fgets(path, sizeof(path), fp) && path[0]) {
                        osd_font = TTF_OpenFont(path, 18);
                    }
                    pclose(fp);
                }
            }
#endif
            if (!osd_font) {
                static const char *const fallbacks[] = {
#if defined(_WIN32)
                    "C:\\Windows\\Fonts\\segoeui.ttf",
                    "C:\\Windows\\Fonts\\arial.ttf",
                    "C:\\Windows\\Fonts\\verdana.ttf",
#elif defined(__APPLE__)
                    "/System/Library/Fonts/SFNS.ttf",
                    "/System/Library/Fonts/Helvetica.ttc",
                    "/Library/Fonts/Arial.ttf",
#else
#endif
                    NULL,
                };
                for (int fi = 0; fallbacks[fi] && !osd_font; fi++) {
                    osd_font = TTF_OpenFont(fallbacks[fi], 18);
                }
            }
        }

        /* Show the window early so the swapchain is fully initialized. */
        SDL_ShowWindow(window);
        if (vk_renderer) {
            int vk_w, vk_h;
            SDL_GetWindowSize(window, &vk_w, &vk_h);
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
