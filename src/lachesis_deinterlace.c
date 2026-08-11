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

#include <libavutil/common.h>
#include <libavutil/frame.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/deinterlacing.h>

#include "lachesis_deinterlace.h"
#include "lachesis_internal.h"
#include "lachesis_options.h"
#include "lachesis_present.h"

int deinterlace = 0;

int deinterlace_active(const VideoState *is) {
    return is->deint_active;
}

/* A new picture always starts over from its first field. */
void deinterlace_new_picture(VideoState *is, const Frame *vp) {
    if (vp->id != is->deint_frame_id) {
        is->deint_frame_id = vp->id;
        is->deint_second_field = 0;
    }
}

void deinterlace_prepare(VideoState *is, Frame *vp) {
    is->render_params.deinterlace = deinterlace;
    is->render_params.prev_frame = NULL;
    is->render_params.next_frame = NULL;
    is->render_params.second_field = is->deint_second_field;
    is->deint_active = deinterlace;

    if (!is->deint_active) {
        return;
    }

    if (is->deint_prev && is->deint_prev->width > 0 &&
        is->deint_prev_serial == vp->serial) {
        is->render_params.prev_frame = is->deint_prev;
    }
    if (frame_queue_nb_remaining(&is->pictq) > 0) {
        Frame *nextvp = frame_queue_peek(&is->pictq);

        if (nextvp != vp && nextvp->serial == vp->serial) {
            is->render_params.next_frame = nextvp->frame;
        }
    }
}

void deinterlace_clear(VideoState *is) {
    is->render_params.deinterlace = 0;
    is->deint_active = 0;
}

/* Keeps the frame about to leave the queue around as YADIF's past reference. */
void deinterlace_retire_frame(VideoState *is) {
    Frame *vp;

    if (!is->pictq.rindex_shown) {
        return;
    }
    vp = frame_queue_peek_last(&is->pictq);
    if (!deinterlace || !vp->frame || !vp->frame->buf[0]) {
        av_frame_unref(is->deint_prev);
        return;
    }
    if (!is->deint_prev) {
        is->deint_prev = av_frame_alloc();
    }
    if (is->deint_prev && av_frame_replace(is->deint_prev, vp->frame) == 0) {
        is->deint_prev_serial = vp->serial;
    }
}

void deinterlace_pace(VideoState *is, double delay, double time,
                      double *remaining_time) {
    double ideal, target, lead = 0;

    if (benchmark || is->paused || !is->deint_active || is->deint_second_field) {
        return;
    }

    ideal = is->frame_timer + delay / 2.0;
    target = present_snap(ideal, time);
    if (target != ideal) {
        lead = FFMIN(PRESENT_LEAD_MAX, present_vsync_sec() * 0.25);
    }
    if (time < target - lead) {
        *remaining_time = FFMIN(target - lead - time, *remaining_time);
        return;
    }

    is->deint_second_field = 1;
    is->force_refresh = 1;
}

void deinterlace_close(VideoState *is) {
    av_frame_free(&is->deint_prev);
}

void deinterlace_apply(struct pl_frame *pl_frame,
                       struct pl_render_params *pl_params,
                       const AVFrame *frame, const RenderParams *params) {
    static const struct pl_deinterlace_params deint = {
        .algo = PL_DEINTERLACE_YADIF,
    };
    /* See libplacebo's validate_structs() if there are mysterious failures. */
    enum pl_field first = PL_FIELD_TOP;

    if (!params->deinterlace) {
        return;
    }
    if ((frame->flags & AV_FRAME_FLAG_INTERLACED) &&
        !(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {
        first = PL_FIELD_BOTTOM;
    }

    pl_frame->first_field = first;
    pl_frame->field = params->second_field ? pl_field_other(first) : first;
    pl_params->deinterlace_params = &deint;
}
