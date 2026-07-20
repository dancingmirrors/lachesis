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

#ifndef LACHESIS_INTERNAL_H
#define LACHESIS_INTERNAL_H

#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/frame.h>
#include <libavutil/macros.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/tx.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include "lachesis_renderer.h"

#define FFP_MIX_MAXVOLUME 128
#define VOLUME_BOOST_MAX_PCT 260
/* No A/V sync correction is done if below this threshold. */
#define AV_NOSYNC_THRESHOLD 10.0
/* Number of audio samples over which the audio difference average is computed. */
#define AUDIO_DIFF_AVG_NB 20

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
    AV_SYNC_EXTERNAL_CLOCK,
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
    int audio_volume_max;
    int muted;
    struct AudioParams audio_src;
    struct AudioParams audio_filter_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;
    int decode_behind_streak;
    int decode_recover_streak;
    int decode_degraded;
    int64_t last_catchup_us;
    int render_low_quality;

    RenderParams render_params;
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
    struct YtdlChunkedIO *ytdl_vio;
    struct YtdlChunkedIO *ytdl_aio;
    AVFormatContext *audio_ic;
    SDL_Thread *audio_read_tid;
    volatile int audio_seek_pending;
    int64_t audio_seek_pos;
    int64_t audio_seek_min;
    int64_t audio_seek_max;
    int audio_seek_flags;

    AVFormatContext *sub_ic;
    SDL_Thread *sub_read_tid;
    int sub_ext_stream;
    volatile int sub_seek_pending;
    int64_t sub_seek_pos;
    int64_t sub_seek_min;
    int64_t sub_seek_max;
    int sub_seek_flags;
    int64_t diag_t0_us;
    int diag_first_vpts_logged;
    int diag_first_apts_logged;
    int diag_first_vpkt_logged;
    int diag_first_apkt_logged;
    int64_t diag_last_pace_us;
    int is_still_image;
    int width, height, xleft, ytop;
    int step;
    int start_pause_pending;
    int begin_paused;

    int vfilter_idx;
    AVFilterContext *in_video_filter; /* The first filter in the video chain. */
    AVFilterContext *out_video_filter; /* The last filter in the video chain. */
    AVFilterContext *in_audio_filter; /* The first filter in the audio chain. */
    AVFilterContext *out_audio_filter; /* The last filter in the audio chain. */
    AVFilterGraph *agraph;

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_Condition *continue_read_thread;
} VideoState;

extern SDL_Window *window;
extern SDL_Renderer *renderer;
extern VkRenderer *vk_renderer;

/* Adding an entry without bumping this is a compile error. */
#define SDL_TEXTURE_FORMAT_MAP_SIZE 19
struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
};
extern const struct TextureFormatEntry sdl_texture_format_map[SDL_TEXTURE_FORMAT_MAP_SIZE];
extern const SDL_PixelFormat *renderer_texture_formats;

int realloc_texture(SDL_Texture **texture, Uint32 new_format,
                    int new_width, int new_height,
                    SDL_BlendMode blendmode, int init_texture);

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

extern double ab_loop_a;
extern double ab_loop_b;
int ab_loop_defining(void);

double get_master_clock(VideoState *is);
Frame *frame_queue_peek(FrameQueue *f);
Frame *frame_queue_peek_last(FrameQueue *f);
int frame_queue_nb_remaining(FrameQueue *f);
int64_t frame_queue_last_pos(FrameQueue *f);

int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);
void packet_queue_flush(PacketQueue *q);
int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_Condition *empty_queue_cond);
int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg);
void decoder_destroy(Decoder *d);
void decoder_abort(Decoder *d, FrameQueue *fq);
int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
int video_thread(void *arg);
void apply_degraded_decode(AVCodecContext *avctx);
void set_default_window_size(int width, int height, AVRational sar);
Frame *frame_queue_peek_writable(FrameQueue *f);
Frame *frame_queue_peek_readable(FrameQueue *f);
void frame_queue_push(FrameQueue *f);
void frame_queue_next(FrameQueue *f);
double get_clock(Clock *c);
void set_clock(Clock *c, double pts, int serial);
void set_clock_at(Clock *c, double pts, int serial, double time);
void sync_clock_to_slave(Clock *c, Clock *slave);
int get_master_sync_type(VideoState *is);
int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                          AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

void calculate_display_rect(SDL_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar);

#define FF_QUIT_EVENT (SDL_EVENT_USER + 2)

#define FF_QUIT_REASON_EOF 0
#define FF_QUIT_REASON_ERROR 1

#define PLAYBACK_SPEED_STEP 0.1

extern float display_scale;
extern float display_pan_x;
extern float display_pan_y;
extern enum Vk360Layout view360_layout;
extern float sbs360_yaw;
extern float sbs360_pitch;
extern float sbs360_hfov;
void sbs360_reset_view(void);
extern int deinterlace;
extern double playback_speed;
extern int screen_width;
extern int screen_height;
extern int default_width;
extern int default_height;
extern int cursor_hidden;
extern int64_t cursor_last_shown;
extern int fatal_error_pending;
extern int playlist_pos;
extern int playlist_size;
extern int playlist_nav_dir;

void do_exit(VideoState *is);
void toggle_pause(VideoState *is);
void toggle_mute(VideoState *is);
void update_volume(VideoState *is, int sign, double step);
void step_to_next_frame(VideoState *is);
void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes);
void stream_cycle_channel(VideoState *is, int codec_type);
void set_playback_speed(double speed);
void ab_loop_toggle(VideoState *is);
void toggle_fullscreen(VideoState *is);
void playlist_switch(VideoState **pis, int new_pos);
void playlist_remove_current(VideoState **pis, int keep_paused);
void refresh_loop_wait_event(VideoState *is, SDL_Event *event);

#endif /* LACHESIS_INTERNAL_H */
