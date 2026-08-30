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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_filters.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 11, 100)
static enum AVAlphaMode supported_alpha_modes[] = {
    AVALPHA_MODE_UNSPECIFIED,
    AVALPHA_MODE_STRAIGHT,
};
#endif

int check_filtergraph(const char *desc) {
    if (!desc || !*desc) {
        return 0;
    }

    AVFilterGraph *graph = avfilter_graph_alloc();
    if (!graph) {
        /* "Out of memory" doesn't count. */
        return 0;
    }

    AVFilterInOut *inputs = NULL, *outputs = NULL;
    int ret = avfilter_graph_parse2(graph, desc, &inputs, &outputs);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);

    return ret;
}

static double get_rotation(const int32_t *displaymatrix) {
    double theta = 0;
    if (displaymatrix) {
        theta = -round(av_display_rotation_get(displaymatrix));
    }

    theta -= 360 * floor(theta / 360 + 0.9 / 360);

    return theta;
}

int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame, int force_autoscale) {
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVFilterContext *autoscale_ctx = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    int max_dim;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par) {
        return AVERROR(ENOMEM);
    }

    if (!slow) {
        graph->scale_sws_opts = av_strdup("flags=fast_bilinear");
    }
    alloc_track_disown(graph->scale_sws_opts);

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

    {
        int nb_pix_fmts = 0;
        const enum AVPixelFormat *pix_fmts =
            renderer_supported_pixfmts(renderer, &nb_pix_fmts);

        if (pix_fmts && nb_pix_fmts > 0 &&
            (ret = av_opt_set_array(filt_out, "pixel_formats",
                                    AV_OPT_SEARCH_CHILDREN, 0, nb_pix_fmts,
                                    AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0) {
            goto fail;
        }
    }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 11, 100)
    if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(supported_alpha_modes),
                                AV_OPT_TYPE_INT, supported_alpha_modes)) < 0) {
        goto fail;
    }
#endif

    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0) {
        goto fail;
    }

    last_filter = filt_out;

    /* clang-format off */
#define INSERT_FILT(name, arg)                                                  \
    do {                                                                        \
        AVFilterContext *filt_ctx;                                              \
                                                                                \
        ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                           avfilter_get_by_name(name),          \
                                           "lachesis_" name, arg, NULL, graph); \
        if (ret < 0)                                                            \
            goto fail;                                                          \
                                                                                \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                       \
        if (ret < 0)                                                            \
            goto fail;                                                          \
                                                                                \
        last_filter = filt_ctx;                                                 \
    } while (0)
    /* clang-format on */

    /* INSERT_FILT() builds the chain backwards. */
    max_dim = display_max_texture_size();
    if (max_dim > 0 && (force_autoscale || frame->width > max_dim || frame->height > max_dim)) {
        int scale_ret;
        char scale_buf[256];

        snprintf(scale_buf, sizeof(scale_buf),
                 "w=if(gt(max(iw,ih),%d),max(2,2*floor(iw*%d/max(iw,ih)/2)),iw)"
                 ":h=if(gt(max(iw,ih),%d),max(2,2*floor(ih*%d/max(iw,ih)/2)),ih)"
                 ":flags=area",
                 max_dim, max_dim, max_dim, max_dim);

        scale_ret = avfilter_graph_create_filter(&autoscale_ctx,
                                                 avfilter_get_by_name("scale"),
                                                 "lachesis_autoscale", scale_buf,
                                                 NULL, graph);
        if (scale_ret >= 0 && autoscale_ctx) {
            scale_ret = avfilter_link(autoscale_ctx, 0, last_filter, 0);
            if (scale_ret < 0) {
                avfilter_free(autoscale_ctx);
            }
        }
        if (scale_ret < 0 || !autoscale_ctx) {
            autoscale_ctx = NULL;
            log_verbose("Failed to create the scale filter.\n");
        } else {
            last_filter = autoscale_ctx;
        }
    }

    if (fps_convert > 0) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "fps=%.6g", fps_convert);
        INSERT_FILT("fps", fps_buf);
    }

    if (autorotate) {
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
        if (displaymatrix) {
            double theta = get_rotation(displaymatrix);

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
            } else if (displaymatrix[4] < 0) {
                INSERT_FILT("vflip", NULL);
            }
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0) {
        goto fail;
    }

    if (autoscale_ctx && autoscale_ctx->nb_inputs && autoscale_ctx->nb_outputs) {
        int in_w = autoscale_ctx->inputs[0]->w;
        int in_h = autoscale_ctx->inputs[0]->h;
        int out_w = autoscale_ctx->outputs[0]->w;
        int out_h = autoscale_ctx->outputs[0]->h;

        if ((in_w != out_w || in_h != out_h) &&
            (is->oversize_warned_w != in_w || is->oversize_warned_h != in_h)) {
            is->oversize_warned_w = in_w;
            is->oversize_warned_h = in_h;
            log_warn("%dx%d exceeds %d. Scaling to %dx%d.\n", in_w, in_h,
                     max_dim, out_w, out_h);
        }
    }

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);

    return ret;
}

int filtergraph_output_oversize(AVFilterContext *filt_out) {
    int max_dim = display_max_texture_size();

    if (!filt_out || max_dim <= 0) {
        return 0;
    }

    return av_buffersink_get_w(filt_out) > max_dim ||
        av_buffersink_get_h(filt_out) > max_dim;
}

void report_filter_output(AVFilterContext *filt_out, const AVFrame *frame,
                          int *last_w, int *last_h, AVRational *last_sar,
                          AVRational *last_fr) {
    int lw = av_buffersink_get_w(filt_out);
    int lh = av_buffersink_get_h(filt_out);
    AVRational ofr = av_buffersink_get_frame_rate(filt_out);
    AVRational osar;
    int ow, oh;
    int max_dim;

    osar = frame->sample_aspect_ratio;
    if (osar.num <= 0 || osar.den <= 0) {
        osar = av_buffersink_get_sample_aspect_ratio(filt_out);
    }

    frame_visible_size(frame, &ow, &oh);

    if (ow == *last_w && oh == *last_h && !av_cmp_q(osar, *last_sar) &&
        ofr.num == last_fr->num && ofr.den == last_fr->den) {
        return;
    }
    *last_w = ow;
    *last_h = oh;
    *last_sar = osar;
    *last_fr = ofr;

    /* The renderer still has to hold the uncropped surface. */
    max_dim = display_max_texture_size();
    if (max_dim > 0 && (lw > max_dim || lh > max_dim)) {
        log_warn("%dx%d exceeds %d.\n", lw, lh, max_dim);
    }

    media_info_note_video_output(ow, oh, osar, ofr);
}
