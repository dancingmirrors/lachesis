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

#include <libavutil/common.h>
#include <libavutil/time.h>

#include "lachesis_deinterlace.h"
#include "lachesis_interpolate.h"
#include "lachesis_options.h"
#include "lachesis_present.h"

#define RATE_MATCH_TOLERANCE 0.01
#define RATE_MATCH_HOLD 5
#define BLEND_SHARE_WINDOW 120
#define REFRESH_LEAD 0.004

enum {
    INTERP_OFF,
    INTERP_IDLE,
    INTERP_DEINTERLACING,
    INTERP_RATE_MATCH,
    INTERP_STARVED,
    INTERP_NO_REFRESH,
    INTERP_ON,
};

static struct {
    int state;
    int serial;
    int hold;
    double drawn_vsync;
    double rate;
    double blend_share;
    int64_t blends_counted;
} interp;

static int interpolation_wanted(const VideoState *is) {
    return frame_interpolation && !benchmark && !display_disable && !is->paused &&
        !is->step && !is->is_still_image && is->video_st &&
        !deinterlace_active(is) && present_vsync_sec() > 0;
}

static int inactive(int state) {
    interp.state = state;
    interp.blend_share = 0.0;
    interp.blends_counted = 0;

    return 0;
}

int interpolate_frames(VideoState *is, Frame *vp, RenderMixFrame *mix,
                       float *vsync_duration) {
    double frame_dur, vsync, vsync_norm, vsync_pts, now, next_vsync, clock;
    Frame *next;

    if (!interpolation_wanted(is)) {
        if (!frame_interpolation) {
            return inactive(INTERP_OFF);
        }
        return inactive(deinterlace_active(is) ? INTERP_DEINTERLACING
                                               : INTERP_IDLE);
    }

    if (vp->serial != interp.serial) {
        interp.serial = vp->serial;
        interp.hold = 0;
    }

    if (frame_queue_nb_remaining(&is->pictq) < 1) {
        return inactive(INTERP_STARVED);
    }
    next = frame_queue_peek(&is->pictq);
    if (next->serial != vp->serial || !(next->pts > vp->pts) ||
        next->width != vp->width || next->height != vp->height ||
        next->format != vp->format) {
        return inactive(INTERP_STARVED);
    }

    frame_dur = next->pts - vp->pts;
    if (frame_dur > is->max_frame_duration) {
        frame_dur = vp->duration;
    }
    if (!(frame_dur > 0.0)) {
        return inactive(INTERP_STARVED);
    }

    vsync = present_vsync_sec();
    interp.rate = playback_speed / frame_dur;
    if (fabs(frame_dur / playback_speed / vsync - 1.0) <= RATE_MATCH_TOLERANCE) {
        interp.hold = RATE_MATCH_HOLD;
    } else if (interp.hold > 0) {
        interp.hold--;
    }
    if (interp.hold > 0) {
        return inactive(INTERP_RATE_MATCH);
    }

    now = av_gettime_relative() / 1000000.0;
    next_vsync = present_next_vsync(now, NULL);
    if (isnan(next_vsync)) {
        return inactive(INTERP_NO_REFRESH);
    }

    clock = get_clock(&is->vidclk);
    if (isnan(clock)) {
        clock = vp->pts;
    }
    vsync_pts = clock + (next_vsync - now) * playback_speed;
    vsync_norm = vsync * playback_speed / frame_dur;

    mix[0] = (RenderMixFrame){
        .frame = vp->frame,
        .signature = vp->id,
        .ts = (float)((vp->pts - vsync_pts) / frame_dur),
    };
    mix[1] = (RenderMixFrame){
        .frame = next->frame,
        .signature = next->id,
        .ts = (float)((next->pts - vsync_pts) / frame_dur),
    };
    *vsync_duration = (float)vsync_norm;

    interp.blend_share +=
        ((mix[1].ts > 0.0f && mix[1].ts < vsync_norm ? 1.0 : 0.0) -
         interp.blend_share) /
        FFMIN(++interp.blends_counted, BLEND_SHARE_WINDOW);
    interp.state = INTERP_ON;

    return 2;
}

int interpolate_pace(VideoState *is, double now, double *remaining_time) {
    double vsync, lead, next_vsync;

    if (interp.state != INTERP_ON || !interpolation_wanted(is) ||
        !is->pictq.rindex_shown) {
        return 0;
    }

    vsync = present_vsync_sec();
    lead = FFMIN(REFRESH_LEAD, vsync / 4.0);
    next_vsync = present_next_vsync(now, NULL);
    if (isnan(next_vsync)) {
        is->force_refresh = 1;
        *remaining_time = 0.0;
        return 0;
    }

    if (next_vsync > interp.drawn_vsync + vsync / 2.0) {
        is->force_refresh = 1;
        interp.drawn_vsync = next_vsync;
        *remaining_time =
            FFMIN(FFMAX(next_vsync - lead - now, 0.0), *remaining_time);
        return 0;
    }

    *remaining_time =
        FFMIN(FFMAX(next_vsync + vsync - lead - now, 0.0), *remaining_time);

    return 1;
}

const char *interpolate_status(void) {
    static char status[64];

    switch (interp.state) {
    case INTERP_ON:
        snprintf(status, sizeof(status), "on, blending %.0f%% of refreshes",
                 100.0 * interp.blend_share);
        return status;
    case INTERP_RATE_MATCH:
        snprintf(status, sizeof(status), "off, %.3g FPS matches the display",
                 interp.rate);
        return status;
    case INTERP_DEINTERLACING:
        return "off, deinterlacing already paints every field";
    case INTERP_STARVED:
        return "waiting for frames";
    case INTERP_NO_REFRESH:
        return "no refresh estimate";
    case INTERP_IDLE:
        return "idle";
    default:
        return "off";
    }
}
