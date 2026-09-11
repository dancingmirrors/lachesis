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

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_audio.h"
#include "lachesis_degrade.h"
#include "lachesis_deinterlace.h"
#include "lachesis_display.h"
#include "lachesis_equalizer.h"
#include "lachesis_internal.h"
#include "lachesis_interpolate.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_present.h"
#include "lachesis_queue.h"
#include "lachesis_renderer.h"
#include "lachesis_seek.h"
#include "lachesis_subtitles.h"
#include "lachesis_view.h"
#include "lachesis_view360.h"
#include "lachesis_window.h"

#define AV_SYNC_SLEW_GAIN 0.1
#define AV_SYNC_SLEW_FACTOR 0.1
#define AV_SYNC_RESYNC_THRESHOLD 0.2
#define AV_SYNC_MAX_CATCHUP 0.25

#define SLOW_OPEN_US (500 * 1000)
#define OSD_ONLY_REFRESH_RATE (1.0 / 30.0)
#define RENDER_FAULT_LIMIT 8
#define RENDER_FAULT_LIMIT_LATE 90

static int display_deferred;
static int render_fail_streak;
static int render_ever_ok;
static int render_fault_event_sent;
static unsigned sub_rgba_generation;

int render_ever_worked(void) {
    return render_ever_ok;
}

void render_fault_forget(void) {
    render_fail_streak = 0;
    render_ever_ok = 0;
    render_fault_event_sent = 0;
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

void apply_present_feedback(void) {
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

static int audio_start_picture_shown(VideoState *is) {
    Frame *shown;

    if (!video_stream_advances(is) || is->audio_start_serial < 0) {
        return 1;
    }
    if (!is->pictq.rindex_shown) {
        return 0;
    }
    shown = frame_queue_peek_last(&is->pictq);

    return shown && shown->serial != is->audio_start_serial;
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

    if (is->audio_start_pending && audio_start_picture_shown(is)) {
        is->audio_start_pending = 0;
        audio_device_resume();
    }

    return display_deferred == owed;
}

static double compute_target_delay(double delay, VideoState *is) {
    double diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);
        is->last_av_diff = isnan(diff) ? LACHESIS_NAN : -diff;
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            double base = delay > 0 ? delay : AV_SYNC_THRESHOLD_MIN;
            int is_near = fabs(diff) <= AV_SYNC_THRESHOLD_MAX;
            double gain = is_near ? AV_SYNC_SLEW_GAIN : 1.0;
            double limit = base * (is_near ? AV_SYNC_SLEW_FACTOR : AV_SYNC_MAX_CATCHUP);

            delay = FFMAX(0, delay + av_clipd(diff * gain, -limit, limit));
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
    external_clock_reseat(is, &is->vidclk);
}

static int open_is_slow(VideoState *is) {
    return SDL_GetAtomicInt(&is->open_phase) != STREAM_OPEN_DONE &&
        av_gettime_relative() - is->open_started_us >= SLOW_OPEN_US;
}

/* The OSD runs on wall clock time so it has to be painted even when the video is not. */
int osd_wants_repaint(VideoState *is, double now) {
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

void video_refresh(void *opaque, double *remaining_time) {
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
                double lead = target != ideal ? present_lead_sec() : 0;
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
            degrade_note_shown(is);
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
