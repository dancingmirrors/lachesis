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

#include "lachesis_math.h"
#include "lachesis_playlist.h"
#include "lachesis_queue.h"
#include "lachesis_renderer.h"

#define FFP_MIX_MAXVOLUME 128
#define VOLUME_BOOST_MAX_PCT 300
/* No A/V sync correction is done if below this threshold. */
#define AV_NOSYNC_THRESHOLD 10.0
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
/* Number of audio samples over which the audio difference average is computed. */
#define AUDIO_DIFF_AVG_NB 20
/* How long audio waits for the first picture at startup and after a seek. */
#define AUDIO_START_MAX_WAIT_US (10 * 1000000)
#define AUDIO_RESYNC_MAX_WAIT_US (2 * 1000000)

#define DEGRADE_NONE 0
#define DEGRADE_FILTER 1
#define DEGRADE_CHEAP 2
#define DEGRADE_SKIP 3
#define DEGRADE_MAX DEGRADE_SKIP

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

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

enum StreamOpenPhase {
    STREAM_OPEN_STARTING,
    STREAM_OPEN_BINDING,
    STREAM_OPEN_DONE,
};

typedef struct VideoState {
    SDL_Thread *read_tid;
    const AVInputFormat *iformat;
    volatile int abort_request;
    SDL_AtomicInt read_thread_done;
    SDL_AtomicInt audio_read_thread_done;
    SDL_AtomicInt sub_read_thread_done;
    volatile int abandoned;
    /* Held while a reader claims something the rest of the process shares. */
    SDL_Mutex *pipeline_mutex;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    /* Bumped by every request so the demuxer can tell a newer one from the request it's already servicing. */
    int seek_serial;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int seek_exact;
    int64_t seek_exact_pts;
    double exact_seek_backoff;
    double exact_seek_backoff_target;
    double exact_seek_pts;
    int exact_seek_video_serial;
    int exact_seek_audio_serial;
    double start_playhead;
    SDL_AtomicInt seek_by_bytes;
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
    double audclk_drift;
    double audclk_drift_time;
    int audclk_drift_serial;
    int audclk_drift_speed_serial;
    int audclk_drift_valid;
    int frame_drops_early;
    int frame_drops_late;
    double decode_span_pts;
    int decode_span_serial;
    int degrade_level;
    int degrade_episodes;
    int degrade_relapses[DEGRADE_MAX + 1];
    int64_t degrade_left_us[DEGRADE_MAX + 1];
    int degrade_warned;
    int64_t degrade_changed_us;
    int64_t degrade_read_ahead_us;
    int64_t degrade_late_since_us;
    double degrade_lag_peak;
    unsigned degrade_shown;
    unsigned degrade_shown_mark;
    int64_t degrade_shown_us;
    int degrade_held;
    int64_t degrade_calm_us;
    int64_t degrade_calm_mark_us;
    double catchup_kept_time;
    double decode_cost;
    int64_t cost_decode_us;
    int64_t cost_budget_us;
    /* How long the video thread spent parked on a full picture queue. */
    int64_t stall_us;
    int64_t stall_mark_us;
    int64_t stall_fold_us;
    double stall_frac;
    int degrade_judder_base;
    int64_t degrade_judder_us;
    double degrade_judder_rate;
    int degrade_serial;
    int degrade_deaf;
    double degrade_speed;
    int degrade_shape;
    int64_t degrade_settle_us;
    int64_t last_content_skip_us;
    double content_skip_pts;
    int content_skip_serial;
    int64_t content_skip_until_us;
    int content_skips;
    int64_t last_catchup_us;

    RenderParams render_params;
    int last_render_serial;
    int deint_active;
    int deint_second_field;
    double deint_frame_dur;
    uint64_t deint_frame_id;
    AVFrame *deint_prev;
    int deint_prev_serial;
    uint64_t deint_prev_id;
    int render_storage_w;
    int render_storage_h;
    uint8_t *sub_rgba;
    int sub_rgba_w;
    int sub_rgba_h;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    double last_av_diff;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    int hwaccel_off;
    /* Maximum duration of a frame, above which we consider the jump a timestamp discontinuity. */
    double max_frame_duration;
    struct SwsContext *sub_convert_ctx;
    int eof;
    int play_range_done;
    int loop_remaining;

    char *filename;
    int from_playlist;
    char *archive_path;
    char *entry_name;
    AVIOContext *archive_avio;
    struct AppendIO *append_io;
    int ytdl_forced;
    char *ytdl_source_url;
    char *ytdl_audio_url;
    struct YtdlChunkedIO *ytdl_vio;
    struct YtdlChunkedIO *ytdl_aio;
    AVFormatContext *audio_ic;
    SDL_Thread *audio_read_tid;
    volatile int audio_range_over;
    volatile int audio_seek_pending;
    int64_t audio_seek_pos;
    int64_t audio_seek_min;
    int64_t audio_seek_max;
    int audio_seek_flags;

    AVFormatContext *sub_ic;
    SDL_Thread *sub_read_tid;
    int sub_ext_stream;
    int64_t sub_ts_offset;
    volatile int sub_abort_request;
    volatile int sub_seek_pending;
    int64_t sub_seek_pos;
    int64_t sub_seek_min;
    int64_t sub_seek_max;
    int sub_seek_flags;
    double observed_length;
    double observed_pos;
    int64_t diag_t0_us;
    int diag_first_vpts_logged;
    int diag_first_apts_logged;
    int diag_first_vpkt_logged;
    int diag_first_apkt_logged;
    int64_t diag_last_pace_us;
    int is_still_image;
    SDL_AtomicInt streams_selected;
    SDL_AtomicInt open_phase;
    int64_t open_started_us;
    double last_draw_time;
    unsigned osd_state;
    int width, height, xleft, ytop;
    int window_opened;
    int step;
    int step_from_play;
    int step_key_held;
    int start_pause_pending;
    int begin_paused;
    int extclk_reseat;
    int audio_start_pending;
    int audio_start_serial;
    int64_t audio_start_deadline_us;
    double audio_catchup_pts;
    int audio_catchup_serial;
    int audio_catchup_checked_serial;
    int audio_catchup_startup;
    int pictq_last_serial;

    int vfilter_idx;
    int oversize_warned_w, oversize_warned_h;
    AVFilterContext *in_video_filter; /* The first filter in the video chain. */
    AVFilterContext *out_video_filter; /* The last filter in the video chain. */
    AVFilterContext *in_audio_filter; /* The first filter in the audio chain. */
    AVFilterContext *out_audio_filter; /* The last filter in the audio chain. */
    AVFilterGraph *agraph;

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_Condition *continue_read_thread;
} VideoState;

extern SDL_Window *window;
extern Renderer *renderer;

int stream_has_enough_packets(const VideoState *is, AVStream *st, int stream_id,
                              PacketQueue *queue);
int video_thread(void *arg);
void apply_degraded_decode(AVCodecContext *avctx, int level);
const char *degrade_status(const VideoState *is);
int note_window_pixel_size(int w, int h);
void update_screen_size(void);
int video_adopt_window_size(VideoState *is);
float window_pixel_density(void);

int display_max_texture_size(void);

static inline void frame_visible_size(const AVFrame *frame, int *w, int *h) {
    int64_t vw = frame->width - (int64_t)frame->crop_left - (int64_t)frame->crop_right;
    int64_t vh = frame->height - (int64_t)frame->crop_top - (int64_t)frame->crop_bottom;

    *w = vw > 0 ? (int)vw : frame->width;
    *h = vh > 0 ? (int)vh : frame->height;
}

static inline void fit_within_max_dim(int w, int h, int max_dim, int *out_w, int *out_h) {
    int64_t m = FFMAX(w, h);
    int64_t sw, sh;

    if (m <= 0 || max_dim <= 0) {
        *out_w = FFMAX(2, w & ~1);
        *out_h = FFMAX(2, h & ~1);
        return;
    }

    sw = FFMIN((int64_t)w, (int64_t)w * max_dim / m);
    sh = FFMIN((int64_t)h, (int64_t)h * max_dim / m);

    *out_w = (int)FFMAX(2, sw & ~(int64_t)1);
    *out_h = (int)FFMAX(2, sh & ~(int64_t)1);
}

void video_prepare_overlays(VideoState *is);

void thread_set_priority(SDL_ThreadPriority priority, const char *who);

#define FF_QUIT_EVENT (SDL_EVENT_USER + 2)
#define FF_RENDER_FAULT_EVENT (SDL_EVENT_USER + 3)
#define FF_SCREENSHOT_EVENT (SDL_EVENT_USER + 4)
#define FF_SINGLE_EVENT (SDL_EVENT_USER + 5)

#define FF_QUIT_REASON_EOF 0
#define FF_QUIT_REASON_ERROR 1

extern enum View360Layout view360_layout;
extern enum View360Projection view360_projection;

static inline int view360_enabled(void) {
    return view360_layout != VIEW360_LAYOUT_OFF;
}

extern float sbs360_yaw;
extern float sbs360_pitch;
extern float sbs360_roll;
extern float sbs360_hfov;
void sbs360_reset_view(void);

extern int deinterlace;
extern enum SupersampleLevel supersample_level;
extern int frame_interpolation;
extern int screen_width;
extern int screen_height;
extern int default_width;
extern int default_height;
extern int cursor_hidden;
extern int64_t cursor_last_shown;
extern int fatal_error_pending;
extern int exit_status;

int pipeline_setup_begin(VideoState *is);
void pipeline_setup_end(VideoState *is);

av_noreturn void do_exit(VideoState *is);
void toggle_mute(VideoState *is);
void update_volume(VideoState *is, int sign, double step);
void stream_cycle_channel(VideoState *is, int codec_type);
void toggle_fullscreen(VideoState *is);
void note_fullscreen_state(VideoState *is);
void note_window_resized(VideoState *is, int w, int h);
void window_want_raise(const char *token);
void present_pacing_reset(void);
void note_display_info_change(VideoState *is);
int pause_to_carry(const VideoState *is);
void playlist_switch(VideoState **pis, int new_pos);
int playlist_close_current(VideoState **pis, double *resume_at);
void playlist_reopen_current(VideoState **pis, int keep_paused, double resume_at);
void playlist_drop_current(VideoState **pis, int keep_paused);
void render_fault_fallback(VideoState **pis);
void refresh_loop_wait_event(VideoState *is, SDL_Event *event);

#endif /* LACHESIS_INTERNAL_H */
