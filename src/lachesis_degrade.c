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

#include "lachesis_degrade.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_renderer.h"

#define DEGRADE_LATE_SECS 0.04
#define DEGRADE_STEP_SECS 0.75
#define DEGRADE_STEP_COOLDOWN_US (2 * 1000000)
#define DEGRADE_PANIC_SECS 0.4
#define DEGRADE_PANIC_STEP_SECS 0.2
#define DEGRADE_PANIC_COOLDOWN_US (500 * 1000)

/* XXX: Figure out a reliable heuristic? */
#define DEGRADE_RECOVER_SECS 30.0
#define DEGRADE_SETTLE_US (1 * 1000000)
#define DEGRADE_SANITY_SECS 60.0
#define DEGRADE_CALM_STALL 0.20

#define DEGRADE_RECOVER_MAX_SECS 600.0
#define DEGRADE_EPISODES_MAX 4
#define DEGRADE_RELAPSE_US (2 * 60 * 1000000)
#define DEGRADE_RELAPSE_MAX 4
#define DEGRADE_RENDER_BUDGET 0.4
#define DEGRADE_COST_WINDOW_US 500000
#define DEGRADE_COST_ALPHA 0.25
#define DEGRADE_CATCHUP_COST 0.9

#define DEGRADE_JUDDER_PER_SEC 2.0
#define DEGRADE_JUDDER_WINDOW_US (2 * 1000000)

#define CATCHUP_MAX_FREEZE_SECS 0.25
#define CONTENT_SKIP_LAND_MARGIN 0.1
#define CATCHUP_BEHIND_SECS 5.0
#define CATCHUP_COOLDOWN_US (5 * 1000000)

#define CONTENT_SKIP_MIN_LAG 0.5
#define CONTENT_SKIP_MAX_JUMP 4.0
#define CONTENT_SKIP_SCAN_MAX 2048
#define CONTENT_SKIP_HURRY_SECS 0.2
#define CONTENT_SKIP_HURRY_COOLDOWN_US (20 * 1000)

static int degrade_floor(void) {
    return fast ? DEGRADE_NONREF : DEGRADE_NONE;
}

static double packet_queue_skip_to_keyframe(PacketQueue *q, AVRational tb,
                                            double land_before,
                                            double max_jump) {
    MyAVPacketList pkt1;
    double head = NAN, best = NAN;
    double dropped = 0.0;
    size_t n, target = 0;

    SDL_LockMutex(q->mutex);
    n = av_fifo_can_read(q->pkt_list);
    if (n > CONTENT_SKIP_SCAN_MAX) {
        n = CONTENT_SKIP_SCAN_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        double pts;

        if (av_fifo_peek(q->pkt_list, &pkt1, 1, i) < 0) {
            break;
        }
        if (pkt1.serial != q->serial || !pkt1.pkt->data) {
            break;
        }
        if (pkt1.pkt->pts != AV_NOPTS_VALUE) {
            pts = pkt1.pkt->pts * av_q2d(tb);
        } else if (pkt1.pkt->dts != AV_NOPTS_VALUE) {
            pts = pkt1.pkt->dts * av_q2d(tb);
        } else {
            continue;
        }
        if (isnan(head)) {
            head = pts;
        }
        if (pts - head > max_jump || pts > land_before) {
            break;
        }
        if (i > 0 && (pkt1.pkt->flags & AV_PKT_FLAG_KEY)) {
            best = pts;
            target = i;
            break;
        }
    }
    for (size_t i = 0; i < target; i++) {
        if (av_fifo_read(q->pkt_list, &pkt1, 1) < 0) {
            break;
        }
        q->nb_packets--;
        q->size -= pkt1.pkt->size + sizeof(pkt1);
        q->duration -= pkt1.pkt->duration;
        av_packet_free(&pkt1.pkt);
    }
    if (target > 0 && !isnan(best) && !isnan(head)) {
        dropped = best - head;
    }
    SDL_UnlockMutex(q->mutex);

    return dropped;
}

void apply_degraded_decode(AVCodecContext *avctx, int level) {
    avctx->skip_loop_filter =
        level >= DEGRADE_FILTER ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    avctx->skip_frame =
        level >= DEGRADE_NONREF ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

static const char *degrade_level_name(int level) {
    switch (level) {
    case DEGRADE_RENDER:
        return "1/4";
    case DEGRADE_FILTER:
        return "2/4";
    case DEGRADE_NONREF:
        return "3/4";
    case DEGRADE_SKIP:
        return "4/4 hopeless";
    default:
        return "full quality";
    }
}

const char *degrade_status(const VideoState *is) {
    static char line[192];
    int n = snprintf(line, sizeof(line), "%s",
                     degrade_level_name(is->degrade_level));

    if (is->decode_cost >= 0.0 && n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - n,
                      " (decode thread: %.0f%% busy, %.0f%% idle)",
                      is->decode_cost * 100.0, is->stall_frac * 100.0);
    }
    if (is->content_skips && n > 0 && (size_t)n < sizeof(line)) {
        snprintf(line + n, sizeof(line) - n, ", %d skips", is->content_skips);
    }

    return line;
}

static int64_t degrade_recover_us(const VideoState *is) {
    double secs = DEGRADE_RECOVER_SECS;
    int steps = FFMIN(is->degrade_episodes, DEGRADE_EPISODES_MAX) - 1 +
        is->degrade_relapses[is->degrade_level];

    for (int i = 0; i < steps && secs < DEGRADE_RECOVER_MAX_SECS; i++) {
        secs *= 2.0;
    }

    return (int64_t)(FFMIN(secs, DEGRADE_RECOVER_MAX_SECS) * 1000000.0);
}

static int degrade_render_fits(int64_t interval_us) {
    double acquire = 0.0, render = 0.0, present = 0.0;

    if (interval_us <= 0 || display_disable || !renderer ||
        !renderer_frame_stats(renderer, &acquire, &render, &present)) {
        return 1;
    }

    return (acquire + render + present) * 1000.0 <
        interval_us * DEGRADE_RENDER_BUDGET;
}

static void degrade_set_level(VideoState *is, int level) {
    level = av_clip(level, degrade_floor(), DEGRADE_MAX);
    if (level == is->degrade_level) {
        return;
    }

    if (level > is->degrade_level) {
        if (!is->degrade_warned) {
            is->degrade_warned = 1;
            log_warn("Degraded decoding engaged. Quality will suffer.\n");
        }
        if (level > degrade_floor()) {
            is->degrade_episodes++;
        }
        if (level == is->degrade_left_level &&
            av_gettime_relative() - is->degrade_left_us < DEGRADE_RELAPSE_US &&
            is->degrade_relapses[level] < DEGRADE_RELAPSE_MAX) {
            is->degrade_relapses[level]++;
        }
    } else {
        is->degrade_left_level = is->degrade_level;
        is->degrade_left_us = av_gettime_relative();
    }
    is->degrade_level = level;
    is->render_low_quality = level >= DEGRADE_RENDER;
    is->degrade_changed_us = av_gettime_relative();
    is->degrade_late_since_us = 0;
    is->degrade_calm_since_us = 0;
    if (is->viddec.avctx) {
        apply_degraded_decode(is->viddec.avctx, level);
    }
    log_verbose("Degraded decoding at level %d (%s).\n", level,
                degrade_level_name(level));
}

static void degrade_meters(VideoState *is, int64_t now) {
    int64_t elapsed = now - is->stall_mark_us;

    if (elapsed >= 250000) {
        double frac = (double)(is->stall_us - is->stall_fold_us) / elapsed;

        is->stall_frac += (av_clipd(frac, 0.0, 1.0) - is->stall_frac) * 0.25;
        is->stall_mark_us = now;
        is->stall_fold_us = is->stall_us;
    }
    if (now - is->degrade_judder_us >= DEGRADE_JUDDER_WINDOW_US) {
        double secs = (now - is->degrade_judder_us) / 1000000.0;

        is->degrade_judder_rate =
            (is->frame_drops_late - is->degrade_judder_base) / secs;
        is->degrade_judder_base = is->frame_drops_late;
        is->degrade_judder_us = now;
    }
}

static void degrade_content_skip(VideoState *is, double lag) {
    int64_t now = av_gettime_relative();
    double m, dropped;

    int hurry = lag > CONTENT_SKIP_HURRY_SECS;
    int64_t cooldown =
        hurry ? CONTENT_SKIP_HURRY_COOLDOWN_US : CATCHUP_COOLDOWN_US;

    if (is->degrade_level < DEGRADE_SKIP || is->paused || is->step ||
        is->seek_req || is->viddec.pkt_serial != is->videoq.serial ||
        lag < CONTENT_SKIP_MIN_LAG ||
        now - is->last_content_skip_us < cooldown) {
        return;
    }
    m = get_master_clock(is);
    if (isnan(m)) {
        return;
    }
    is->last_content_skip_us = now;

    dropped = packet_queue_skip_to_keyframe(&is->videoq,
                                            is->video_st->time_base,
                                            m - CONTENT_SKIP_LAND_MARGIN,
                                            FFMAX(CONTENT_SKIP_MAX_JUMP, lag));
    if (dropped <= 0.0) {
        /* XXX: Nothing to land on in what we have buffered. */
        double v = get_clock(&is->vidclk);

        double behind =
            skip_to_keyframe ? CONTENT_SKIP_MIN_LAG : CATCHUP_BEHIND_SECS;

        if (is->audio_st && get_master_sync_type(is) == AV_SYNC_AUDIO_MASTER &&
            !isnan(v) && m - v > behind) {
            stream_seek(is, (int64_t)(m * AV_TIME_BASE),
                        (int64_t)((m - v) * AV_TIME_BASE), 0);
        }
        return;
    }

    if (is->viddec.packet_pending) {
        is->viddec.packet_pending = 0;
        av_packet_unref(is->viddec.pkt);
    }
    avcodec_flush_buffers(is->viddec.avctx);
    is->decode_span_pts = NAN;
    is->content_skips++;
    SDL_SignalCondition(is->continue_read_thread);
    log_verbose("Skipped %.2fs of video to the next keyframe.\n", dropped);
}

static void degrade_update(VideoState *is, double dpts,
                           int64_t interval_us) {
    int64_t now = av_gettime_relative();
    double master, lag;
    int late, calm, panic, judder;

    if (get_master_sync_type(is) == AV_SYNC_VIDEO_MASTER || is->paused ||
        is->step || benchmark || isnan(dpts) || !video_stream_advances(is) ||
        is->viddec.pkt_serial != is->vidclk.serial ||
        now - is->degrade_serial_us < DEGRADE_SETTLE_US) {
        goto unknown;
    }
    master = get_master_clock(is);
    if (isnan(master)) {
        goto unknown;
    }
    lag = master - dpts;
    if (lag < -AV_NOSYNC_THRESHOLD || lag > DEGRADE_SANITY_SECS) {
        goto unknown;
    }

    judder = is->degrade_judder_rate > DEGRADE_JUDDER_PER_SEC &&
        lag > -DEGRADE_LATE_SECS;
    late = lag > DEGRADE_LATE_SECS || is->decode_cost > 1.0 || judder;
    calm = !judder && lag < DEGRADE_LATE_SECS / 2 &&
        is->stall_frac > DEGRADE_CALM_STALL && is->decode_cost >= 0.0 &&
        is->decode_cost * (2 + is->degrade_level) < 1.0;
    panic = lag > DEGRADE_PANIC_SECS;

    if (late) {
        is->degrade_calm_since_us = 0;
        if (!is->degrade_late_since_us) {
            is->degrade_late_since_us = now;
        }
        int hurried = panic && is->degrade_level + 1 < DEGRADE_SKIP;
        double need = hurried ? DEGRADE_PANIC_STEP_SECS : DEGRADE_STEP_SECS;
        int64_t cooldown =
            hurried ? DEGRADE_PANIC_COOLDOWN_US : DEGRADE_STEP_COOLDOWN_US;

        if (is->degrade_level < DEGRADE_MAX &&
            now - is->degrade_late_since_us >= (int64_t)(need * 1000000.0) &&
            now - is->degrade_changed_us >= cooldown &&
            (is->degrade_level + 1 < DEGRADE_SKIP ||
             lag >= CONTENT_SKIP_MIN_LAG)) {
            degrade_set_level(is, is->degrade_level + 1);
        }
    } else if (calm) {
        is->degrade_late_since_us = 0;
        if (!is->degrade_calm_since_us) {
            is->degrade_calm_since_us = now;
        }
        if (is->degrade_level > degrade_floor() &&
            now - is->degrade_calm_since_us >= degrade_recover_us(is) &&
            (is->degrade_level != DEGRADE_RENDER ||
             degrade_render_fits(interval_us))) {
            degrade_set_level(is, is->degrade_level - 1);
        }
    } else {
        is->degrade_late_since_us = 0;
        is->degrade_calm_since_us = 0;
    }

    degrade_content_skip(is, lag);

    return;

unknown:
    is->degrade_late_since_us = 0;
    is->degrade_calm_since_us = 0;
}

void degrade_init(VideoState *is) {
    is->degrade_serial = -1;
    is->degrade_left_level = -1;
    is->decode_cost = -1.0;
    is->stall_mark_us = is->degrade_serial_us = av_gettime_relative();
    if (degrade_floor() != DEGRADE_NONE) {
        degrade_set_level(is, degrade_floor());
    }
}

void degrade_reset(VideoState *is) {
    is->degrade_level = degrade_floor();
    is->render_low_quality = is->degrade_level >= DEGRADE_RENDER;
    is->degrade_episodes = 0;
    memset(is->degrade_relapses, 0, sizeof(is->degrade_relapses));
    is->degrade_left_level = -1;
    is->degrade_warned = 0;
    is->degrade_serial = -1;
    is->decode_cost = -1.0;
    is->cost_decode_us = 0;
    is->cost_budget_us = 0;
    is->stall_frac = 0.0;
    is->degrade_judder_rate = 0.0;
}

void degrade_note_stall(VideoState *is, int64_t stall_us) {
    is->stall_us += stall_us;
}

int degrade_can_catch_up(const VideoState *is, double now) {
    return is->decode_cost >= 0.0 && is->decode_cost < DEGRADE_CATCHUP_COST &&
        now - is->catchup_kept_time < CATCHUP_MAX_FREEZE_SECS;
}

void degrade_frame(VideoState *is, double dpts, int64_t decode_us,
                   int64_t budget_us, int64_t interval_us, int had_packets) {
    int64_t now = av_gettime_relative();

    /* A seek invalidates every measurement, but not the verdict. */
    if (is->viddec.pkt_serial != is->degrade_serial) {
        is->degrade_serial = is->viddec.pkt_serial;
        is->degrade_serial_us = now;
        is->decode_cost = -1.0;
        is->cost_decode_us = 0;
        is->cost_budget_us = 0;
        is->degrade_late_since_us = 0;
        is->degrade_calm_since_us = 0;
        is->stall_mark_us = now;
        is->stall_fold_us = is->stall_us;
        is->degrade_judder_us = now;
        is->degrade_judder_base = is->frame_drops_late;
    }
    degrade_meters(is, now);

    if (had_packets && budget_us > 0) {
        is->cost_decode_us += decode_us;
        is->cost_budget_us += budget_us;
    }
    if (is->cost_budget_us >= DEGRADE_COST_WINDOW_US) {
        double cost = (double)is->cost_decode_us / is->cost_budget_us;

        is->decode_cost = is->decode_cost >= 0.0
            ? is->decode_cost + (cost - is->decode_cost) * DEGRADE_COST_ALPHA
            : cost;
        is->cost_decode_us = 0;
        is->cost_budget_us = 0;
    }
    degrade_update(is, dpts, interval_us);
}

int degrade_drop_late_frame(VideoState *is, double dpts, int64_t interval_us) {
    double now = av_gettime_relative() / 1000000.0;
    double diff, slack;

    if (get_master_sync_type(is) == AV_SYNC_VIDEO_MASTER) {
        return 0;
    }
    if (!isnan(dpts)) {
        diff = dpts - get_master_clock(is);
        slack = interval_us > 0 ? interval_us / 1000000.0
                                : AV_SYNC_THRESHOLD_MIN;
        if (!isnan(diff) && diff > -DEGRADE_SANITY_SECS &&
            diff - is->frame_last_filter_delay * playback_speed < -slack &&
            is->viddec.pkt_serial == is->vidclk.serial &&
            is->videoq.nb_packets &&
            (frame_queue_nb_remaining(&is->pictq) > 0 ||
             degrade_can_catch_up(is, now))) {
            is->frame_drops_early++;
            return 1;
        }
    }
    is->catchup_kept_time = now;

    return 0;
}
