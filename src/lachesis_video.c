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
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/film_grain_params.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include <SDL3/SDL.h>

#include "lachesis_degrade.h"
#include "lachesis_filters.h"
#include "lachesis_hwaccel.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_queue.h"
#include "lachesis_seek.h"

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    static uint64_t next_frame_id = 1;
    int64_t stall_t0 = av_gettime_relative();
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq))) {
        return -1;
    }
    degrade_note_stall(is, av_gettime_relative() - stall_t0);

    vp->sar = src_frame->sample_aspect_ratio;

    frame_visible_size(src_frame, &vp->width, &vp->height);
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;
    vp->id = next_frame_id++;

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

static void hwframe_download_inplace(HwDownload *dl, AVFrame *frame) {
    static int warned = 0;
    static int announced = 0;
    AVFrame *sw = av_frame_alloc();
    int ret;

    if (!sw) {
        return;
    }

    ret = hwdownload_frame(dl, sw, frame);
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
        double dpts = LACHESIS_NAN;
        int64_t decode_us = av_gettime_relative() - decode_t0 - is->viddec.wait_us;
        if (decode_us < 0) {
            decode_us = 0;
        }

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        if (exact_seek_drop_video(is, dpts)) {
            av_frame_unref(frame);
            return 0;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
        int64_t interval_us =
            (fr.num > 0 && fr.den > 0) ? (int64_t)(1000000.0 * fr.den / fr.num) : 0;
        int64_t span_us = interval_us;
        if (!isnan(dpts)) {
            if (is->viddec.pkt_serial == is->decode_span_serial &&
                !isnan(is->decode_span_pts)) {
                double span = dpts - is->decode_span_pts;
                if (span > 0.0 && span < 1.0) {
                    span_us = (int64_t)(span * 1000000.0);
                }
            }
            is->decode_span_pts = dpts;
            is->decode_span_serial = is->viddec.pkt_serial;
        }
        int64_t budget_us = playback_speed > 0.0
            ? (int64_t)(span_us / playback_speed)
            : span_us;

        degrade_frame(is, dpts, decode_us, budget_us, had_packets);

        if (degrade_drop_late_frame(is, dpts, interval_us)) {
            av_frame_unref(frame);
            got_picture = 0;
        }
    }

    return got_picture;
}

static void drop_resized_film_grain(AVFrame *frame) {
    static int warned = 0;
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
    const AVFilmGrainParams *fgp;

    if (!sd) {
        return;
    }
    fgp = (const AVFilmGrainParams *)sd->data;
    if (fgp->width <= 0 || fgp->height <= 0 ||
        (fgp->width == frame->width && fgp->height == frame->height)) {
        return;
    }
    av_frame_remove_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
    if (!warned) {
        warned = 1;
        log_verbose("Dropping film grain because the filters resized %dx%d to %dx%d.\n",
                    fgp->width, fgp->height, frame->width, frame->height);
    }
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
    HwDownload download = {0};

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    thread_set_priority(SDL_THREAD_PRIORITY_NORMAL, "video decoder");

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

            media_info_note_hw_frame(is_hw);

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
                hwframe_download_inplace(&download, frame);
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
                log_dead("Failed to configure the video filters for %dx%d %s: "
                         "%s.\n",
                         raw_w, raw_h,
                         av_get_pix_fmt_name(raw_format) ? av_get_pix_fmt_name(raw_format) : "?",
                         av_err2str(ret));
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
            hwframe_download_inplace(&download, frame);
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

            drop_resized_film_grain(frame);

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
            pts = (frame->pts == AV_NOPTS_VALUE) ? LACHESIS_NAN : frame->pts * av_q2d(tb);
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
    hwdownload_free(&download);
    av_frame_free(&frame);

    return 0;
}
