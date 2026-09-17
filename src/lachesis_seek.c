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

#include <math.h>
#include <stdio.h>

#include <libavutil/time.h>

#include "lachesis_audio.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_seek.h"

#define EXACT_SEEK_SLACK 0.005
#define EXACT_SEEK_MAX_RUNUP 30.0
#define EXACT_SEEK_BACKOFF 0.5
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

double playback_speed = 1.0;

#define PLAYBACK_SPEED_MIN 0.2
#define PLAYBACK_SPEED_MAX 2.0

double ab_loop_a = LACHESIS_NAN;
double ab_loop_b = LACHESIS_NAN;

int ab_loop_defining(void) {
    return !isnan(ab_loop_a) && isnan(ab_loop_b);
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

void init_clock(Clock *c, int *queue_serial) {
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

double subtitle_playhead(VideoState *is) {
    if (video_stream_advances(is) && !is->is_still_image &&
        is->pictq.rindex_shown) {
        double pts = frame_queue_peek_last(&is->pictq)->pts;

        if (!isnan(pts)) {
            return pts;
        }
    }

    return effective_playhead(is);
}

void check_external_clock_speed(VideoState *is) {
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

static void audio_hold_for_seek(VideoState *is) {
    if (is->audio_stream < 0 || display_disable || !video_stream_advances(is) ||
        audio_spdif_active()) {
        return;
    }
    is->audio_start_pending = 1;
    is->audio_start_serial = is->videoq.serial;
    is->audio_start_deadline_us = av_gettime_relative() + AUDIO_RESYNC_MAX_WAIT_US;
    audio_device_pause();
}

/* Replace whatever is pending rather than dropping the request. */
static void stream_seek_to(VideoState *is, int64_t pos, int64_t rel, int by_bytes,
                           int exact, int64_t exact_pts) {
    audio_hold_for_seek(is);
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

void stream_toggle_pause(VideoState *is) {
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

void ab_loop_reset(void) {
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

void ab_loop_check(VideoState *is) {
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

void reset_playback_speed(void) {
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

void external_clock_reseat(VideoState *is, Clock *slave) {
    double slave_clock = get_clock(slave);

    if (is->extclk_reseat && !isnan(slave_clock)) {
        is->extclk_reseat = 0;
        set_clock(&is->extclk, slave_clock, slave->serial);
        return;
    }
    sync_clock_to_slave(&is->extclk, slave);
}
