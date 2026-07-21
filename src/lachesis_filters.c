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

#include "lachesis_filters.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_options.h"

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

int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame) {
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    int nb_pix_fmts = 0;
    int i;
    size_t j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; renderer_texture_formats && renderer_texture_formats[i] != SDL_PIXELFORMAT_UNKNOWN; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if ((int)renderer_texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }

    graph->scale_sws_opts = av_strdup("flags=fast_bilinear");

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

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);

    return ret;
}

void report_filter_output(VideoState *is, AVFilterContext *filt_out,
                          int *last_w, int *last_h, AVRational *last_sar) {
    int ow = av_buffersink_get_w(filt_out);
    int oh = av_buffersink_get_h(filt_out);
    AVRational osar = av_buffersink_get_sample_aspect_ratio(filt_out);
    int sw = is->video_st->codecpar->width;
    int sh = is->video_st->codecpar->height;
    AVRational ssar = av_guess_sample_aspect_ratio(is->ic, is->video_st, NULL);

    if (ow == *last_w && oh == *last_h && !av_cmp_q(osar, *last_sar)) {
        return;
    }
    *last_w = ow;
    *last_h = oh;
    *last_sar = osar;

    if (ow == sw && oh == sh && !av_cmp_q(osar, ssar)) {
        return;
    }

    media_info_note_video_output(ow, oh, osar);
}
