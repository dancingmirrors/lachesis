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

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_audio.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"

static SDL_AudioDeviceID audio_dev;
static SDL_AudioStream *audio_stream_dev;

static SDL_AudioSpec audio_dev_spec;
static int audio_dev_spec_valid;

static uint8_t *audio_cb_buf;
static unsigned int audio_cb_buf_size;

int64_t audio_callback_time;
int audio_speed_serial = 0;

#define AUDCLK_SMOOTH_ALPHA 0.05
#define AUDCLK_RESET_THRESHOLD 0.3

#define SPDIF_BURST_MAX 131072

typedef struct SpdifContext {
    volatile int active;
    AVFormatContext *muxer;
    enum AVCodecID codec_id;
    int rate;
    int channels;
    int dtshd_rate;
    AVChannelLayout ch_layout;
    int frame_bytes;
    uint8_t burst[SPDIF_BURST_MAX];
    int burst_len;
    int overflowed;
    int mux_errors;
    int bursts_emitted;
    double burst_secs;
    double source_secs;
    int period_check_off;
} SpdifContext;

static SpdifContext spdif;

static const struct {
    const char *name;
    enum AVCodecID id;
    int hd;
} spdif_codec_names[] = {
    {"ac3", AV_CODEC_ID_AC3, 0},
    {"eac3", AV_CODEC_ID_EAC3, 0},
    {"e-ac3", AV_CODEC_ID_EAC3, 0},
    {"dts", AV_CODEC_ID_DTS, 0},
    {"dts-hd", AV_CODEC_ID_DTS, 1},
    {"truehd", AV_CODEC_ID_TRUEHD, 0},
    {"mp1", AV_CODEC_ID_MP1, 0},
    {"mp2", AV_CODEC_ID_MP2, 0},
    {"mp3", AV_CODEC_ID_MP3, 0},
    {"aac", AV_CODEC_ID_AAC, 0},
};

static size_t spdif_element(const char **p) {
    const char *s = *p;
    size_t len;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    len = strcspn(s, ",");
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        len--;
    }
    *p = s;

    return len;
}

static const char *spdif_next_element(const char *p) {
    p += strcspn(p, ",");

    return *p == ',' ? p + 1 : p;
}

int audio_spdif_names_known(const char *list) {
    const char *p = list;

    if (!p || !*p) {
        return 0;
    }

    while (*p) {
        size_t len = spdif_element(&p);
        int known = len == 3 && !av_strncasecmp(p, "all", 3);
        size_t i;

        for (i = 0; !known && i < FF_ARRAY_ELEMS(spdif_codec_names); i++) {
            const char *name = spdif_codec_names[i].name;
            known = strlen(name) == len && !av_strncasecmp(p, name, len);
        }
        if (!known) {
            return 0;
        }
        p = spdif_next_element(p);
    }

    return 1;
}

static int spdif_selected(enum AVCodecID codec_id, int *want_hd) {
    const char *p = audio_spdif_opt;
    int found = 0;

    if (want_hd) {
        *want_hd = 0;
    }
    if (!p) {
        return 0;
    }

    while (*p) {
        size_t len = spdif_element(&p);
        int all = len == 3 && !av_strncasecmp(p, "all", 3);
        size_t i;

        for (i = 0; i < FF_ARRAY_ELEMS(spdif_codec_names); i++) {
            const char *name = spdif_codec_names[i].name;
            if (!all && (strlen(name) != len || av_strncasecmp(p, name, len))) {
                continue;
            }
            if (spdif_codec_names[i].id != codec_id) {
                continue;
            }
            if (all && spdif_codec_names[i].hd) {
                continue;
            }
            found = 1;
            if (want_hd && spdif_codec_names[i].hd) {
                *want_hd = 1;
            }
        }
        p = spdif_next_element(p);
    }

    return found;
}

int audio_spdif_active(void) {
    return spdif.active;
}

static void spdif_warn_unknown_names(void) {
    static int warned;

    if (warned || !audio_spdif_opt || !audio_spdif_opt[0]) {
        return;
    }
    warned = 1;

    if (!audio_spdif_names_known(audio_spdif_opt)) {
        log_warn("Unknown S/PDIF codec '%s'.\n",
                 audio_spdif_opt);
    }
}

static void spdif_muxer_close(void) {
    if (spdif.muxer) {
        if (spdif.muxer->pb) {
            av_freep(&spdif.muxer->pb->buffer);
            avio_context_free(&spdif.muxer->pb);
        }
        avformat_free_context(spdif.muxer);
        spdif.muxer = NULL;
    }
    spdif.burst_len = 0;
    spdif.overflowed = 0;
    spdif.burst_secs = 0;
    spdif.source_secs = 0;
}

static void spdif_close(void) {
    spdif_muxer_close();
    av_channel_layout_uninit(&spdif.ch_layout);
    memset(&spdif, 0, sizeof(spdif));
    media_info_clear_audio_passthrough();
}

static int spdif_write_burst(void *opaque, const uint8_t *buf, int buf_size) {
    SpdifContext *ctx = opaque;
    int left = SPDIF_BURST_MAX - ctx->burst_len;

    if (buf_size > left) {
        ctx->overflowed = 1;
        buf_size = left;
    }
    memcpy(ctx->burst + ctx->burst_len, buf, buf_size);
    ctx->burst_len += buf_size;

    return buf_size;
}

static int spdif_muxer_open(void) {
    AVFormatContext *s = NULL;
    AVDictionary *opts = NULL;
    AVStream *st;
    uint8_t *buffer;
    int ret;

    spdif_muxer_close();

    ret = avformat_alloc_output_context2(&s, NULL, "spdif", NULL);
    if (ret < 0) {
        return ret;
    }
    spdif.muxer = s;

    buffer = av_mallocz(SPDIF_BURST_MAX);
    if (!buffer) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    s->pb = avio_alloc_context(buffer, SPDIF_BURST_MAX, 1, &spdif, NULL,
                               spdif_write_burst, NULL);
    if (!s->pb) {
        av_free(buffer);
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    alloc_track_disown(buffer);
    s->pb->direct = 1;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = spdif.codec_id;
    st->codecpar->sample_rate = spdif.rate;

    if (spdif.dtshd_rate) {
        av_dict_set_int(&opts, "dtshd_rate", spdif.dtshd_rate, 0);
    }
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    av_dict_set(&opts, "spdif_flags", "+be", 0);
#endif

    ret = avformat_write_header(s, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        goto fail;
    }

    return 0;

fail:
    spdif_muxer_close();

    return ret;
}

static int spdif_profile_maybe_hd(int profile) {
    switch (profile) {
    case AV_PROFILE_DTS_HD_HRA:
    case AV_PROFILE_DTS_HD_MA:
#ifdef AV_PROFILE_DTS_HD_MA_X
    case AV_PROFILE_DTS_HD_MA_X:
#endif
#ifdef AV_PROFILE_DTS_HD_MA_X_IMAX
    case AV_PROFILE_DTS_HD_MA_X_IMAX:
#endif
    case AV_PROFILE_UNKNOWN:
        return 1;
    default:
        return 0;
    }
}

static int spdif_output_params(const AVCodecParameters *par, int want_hd) {
    int fs = par->sample_rate > 0 ? par->sample_rate : 48000;

    spdif.codec_id = par->codec_id;
    spdif.channels = 2;
    spdif.dtshd_rate = 0;

    switch (par->codec_id) {
    case AV_CODEC_ID_AC3:
        spdif.rate = fs;
        break;
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
        spdif.rate = fs < 32000 ? 2 * fs : fs;
        break;
    case AV_CODEC_ID_AAC:
        if (par->extradata_size > 0) {
            return 0;
        }
        if (par->profile == AV_PROFILE_AAC_HE ||
            par->profile == AV_PROFILE_AAC_HE_V2) {
            fs /= 2;
        }
        spdif.rate = fs;
        break;
    case AV_CODEC_ID_EAC3:
        spdif.rate = 4 * fs;
        break;
    case AV_CODEC_ID_DTS:
        if (want_hd && fs % 11025 == 0) {
            want_hd = 0;
        }
        if (want_hd && spdif_profile_maybe_hd(par->profile)) {
            spdif.channels = par->profile == AV_PROFILE_DTS_HD_HRA ? 2 : 8;
            spdif.dtshd_rate = spdif.channels * 96000;
            spdif.rate = 192000;
            break;
        }
        if (par->profile == AV_PROFILE_DTS_EXPRESS) {
            return 0;
        }
        while (fs > 48000) {
            fs /= 2;
        }
        spdif.rate = fs;
        break;
    case AV_CODEC_ID_TRUEHD:
        spdif.channels = 8;
        spdif.rate = fs % 11025 == 0 ? 176400 : 192000;
        break;
    default:
        return 0;
    }

    if (spdif.rate <= 0) {
        return 0;
    }
    spdif.frame_bytes = spdif.channels * (int)sizeof(int16_t);
    av_channel_layout_uninit(&spdif.ch_layout);
    av_channel_layout_default(&spdif.ch_layout, spdif.channels);

    return 1;
}

static void spdif_check_burst_period(int64_t pkt_duration, AVRational tb,
                                     int nb_samples) {
    if (spdif.period_check_off) {
        return;
    }
    if (pkt_duration <= 0) {
        spdif.period_check_off = 1;
        return;
    }

    spdif.burst_secs += (double)nb_samples / spdif.rate;
    spdif.source_secs += pkt_duration * av_q2d(tb);

    if (spdif.source_secs < 2.0) {
        return;
    }
    if (fabs(spdif.burst_secs - spdif.source_secs) <=
        0.05 * spdif.source_secs) {
        spdif.burst_secs = 0;
        spdif.source_secs = 0;
        return;
    }
    spdif.period_check_off = 1;
}

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                                 enum AVSampleFormat fmt2, int64_t channel_count2) {
    /* If channel count == 1, planar and non-planar formats are the same. */
    if (channel_count1 == 1 && channel_count2 == 1) {
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    } else {
        return channel_count1 != channel_count2 || fmt1 != fmt2;
    }
}

#define ATEMPO_MIN 0.5
#define ATEMPO_MAX 100.0
#define ATEMPO_MAX_STAGES 8

static void append_atempo_chain(AVBPrint *bp, double speed) {
    double reach_min = ATEMPO_MIN, reach_max = ATEMPO_MAX, step;
    int stages = 1, i;

    while (stages < ATEMPO_MAX_STAGES &&
           (speed < reach_min - 1e-9 || speed > reach_max + 1e-9)) {
        reach_min *= ATEMPO_MIN;
        reach_max *= ATEMPO_MAX;
        stages++;
    }

    step = pow(speed, 1.0 / stages);
    for (i = 0; i < stages; i++) {
        av_bprintf(bp, "%satempo=%.6g", i ? "," : "", step);
    }
}

static const char *build_audio_filters(const char *afilters, AVBPrint *scratch) {
    if (playback_speed == 1.0) {
        return afilters;
    }
    av_bprint_clear(scratch);
    if (afilters && *afilters) {
        av_bprintf(scratch, "%s,", afilters);
    }
    append_atempo_chain(scratch, playback_speed);
    if (!av_bprint_is_complete(scratch)) {
        return afilters;
    }
    return scratch->str;
}

int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format) {
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc())) {
        return AVERROR(ENOMEM);
    }

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    av_opt_set(is->agraph, "aresample_swr_opts", "", 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    snprintf(asrc_args, sizeof(asrc_args),
             "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
             is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
             1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "lachesis_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0) {
        goto end;
    }

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "lachesis_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0) {
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 6, 100)
        static const enum AVSampleFormat sample_fmts[] = {
            AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
        ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,
                                  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
#endif
            goto end;
    }

    if (force_output_format) {
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 6, 100)
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0) {
            goto end;
        }
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0) {
            goto end;
        }
#else
        char layout[256];
        int sample_rates[] = {is->audio_tgt.freq, -1};
        av_channel_layout_describe(&is->audio_tgt.ch_layout, layout, sizeof(layout));
        if ((ret = av_opt_set(filt_asink, "ch_layouts", layout,
                              AV_OPT_SEARCH_CHILDREN)) < 0) {
            goto end;
        }
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates,
                                       -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            goto end;
        }
#endif
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0) {
        goto end;
    }

    {
        AVBPrint fbp;
        const char *effective;
        av_bprint_init(&fbp, 0, AV_BPRINT_SIZE_AUTOMATIC);
        effective = build_audio_filters(afilters, &fbp);
        ret = configure_filtergraph(is->agraph, effective, filt_asrc, filt_asink);
        av_bprint_finalize(&fbp, NULL);
        if (ret < 0) {
            goto end;
        }
    }

    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0) {
        avfilter_graph_free(&is->agraph);
    }
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int spdif_queue_packet(VideoState *is, AVPacket *pkt) {
    AVRational tb = is->audio_st->time_base;
    int64_t pts = pkt->pts;
    int64_t pos = pkt->pos;
    int64_t duration = pkt->duration;
    int nb_samples;
    AVFrame *frame;
    Frame *af;
    int ret;

    if (!spdif.muxer && (ret = spdif_muxer_open()) < 0) {
        return ret;
    }

    spdif.burst_len = 0;
    spdif.overflowed = 0;

    pkt->stream_index = 0;
    pkt->pts = pkt->dts = 0;

    ret = av_write_frame(spdif.muxer, pkt);
    avio_flush(spdif.muxer->pb);
    if (ret < 0) {
        if (spdif.mux_errors == 50 && !spdif.bursts_emitted) {
            log_dead("%s passthrough has produced no output at all.\n", avcodec_get_name(spdif.codec_id));
        }
        return 0;
    }
    if (spdif.overflowed) {
        return 0;
    }

    nb_samples = spdif.burst_len / spdif.frame_bytes;
    if (nb_samples <= 0) {
        spdif_check_burst_period(duration, tb, 0);
        return 0;
    }
    spdif.bursts_emitted++;
    spdif_check_burst_period(duration, tb, nb_samples);

    if (!(af = frame_queue_peek_writable(&is->sampq))) {
        return AVERROR_EXIT;
    }
    frame = af->frame;
    av_frame_unref(frame);
    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = spdif.rate;
    frame->nb_samples = nb_samples;
    if ((ret = av_channel_layout_copy(&frame->ch_layout, &spdif.ch_layout)) < 0) {
        return ret;
    }
    if ((ret = av_frame_get_buffer(frame, 0)) < 0) {
        return ret;
    }
    memcpy(frame->data[0], spdif.burst, (size_t)nb_samples * spdif.frame_bytes);

    af->pts = pts == AV_NOPTS_VALUE ? NAN : pts * av_q2d(tb);
    af->pos = pos;
    af->serial = is->auddec.pkt_serial;
    af->duration = av_q2d((AVRational){nb_samples, spdif.rate});
    frame->pts = pts;

    frame_queue_push(&is->sampq);

    return 0;
}

static int spdif_audio_thread(VideoState *is) {
    Decoder *d = &is->auddec;
    int last_serial = -1;
    int ret = 0;

    for (;;) {
        AVPacket *pkt = d->pkt;
        int64_t wait_t0;

        if (d->queue->nb_packets == 0) {
            SDL_SignalCondition(d->empty_queue_cond);
        }
        wait_t0 = av_gettime_relative();
        d->wait_us = 0;
        if (packet_queue_get(d->queue, pkt, 1, &d->pkt_serial) < 0) {
            break;
        }
        d->wait_us = av_gettime_relative() - wait_t0;

        if (d->queue->serial != d->pkt_serial) {
            av_packet_unref(pkt);
            continue;
        }
        if (d->pkt_serial != last_serial) {
            spdif_muxer_close();
            d->finished = 0;
            last_serial = d->pkt_serial;
        }
        if (!pkt->data) {
            d->finished = d->pkt_serial;
            av_packet_unref(pkt);
            continue;
        }

        ret = spdif_queue_packet(is, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            d->finished = d->pkt_serial;
            break;
        }
    }

    return ret == AVERROR_EXIT ? 0 : ret;
}

int audio_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame;
    Frame *af;
    int last_serial = -1;
    int last_speed_serial = audio_speed_serial;
    double atempo_base_pts = NAN;
    double graph_speed = playback_speed;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (spdif.active) {
        return spdif_audio_thread(is);
    }

    frame = av_frame_alloc();
    if (!frame) {
        return AVERROR(ENOMEM);
    }

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0) {
            goto the_end;
        }

        if (got_frame && frame->sample_rate > 0 &&
            exact_seek_drop_audio(is,
                                  frame->pts == AV_NOPTS_VALUE
                                      ? NAN
                                      : frame->pts / (double)frame->sample_rate,
                                  frame->nb_samples / (double)frame->sample_rate)) {
            av_frame_unref(frame);
            got_frame = 0;
        }

        if (got_frame) {
            reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                               frame->format, frame->ch_layout.nb_channels) ||
                av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                is->audio_filter_src.freq != frame->sample_rate ||
                is->auddec.pkt_serial != last_serial ||
                audio_speed_serial != last_speed_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                is->audio_filter_src.fmt = frame->format;
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0) {
                    goto the_end;
                }
                is->audio_filter_src.freq = frame->sample_rate;
                last_serial = is->auddec.pkt_serial;
                last_speed_serial = audio_speed_serial;
                atempo_base_pts = NAN;
                graph_speed = playback_speed;

                if ((ret = configure_audio_filters(is, afilters_opt, 1)) < 0) {
                    goto the_end;
                }
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0) {
                goto the_end;
            }

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq))) {
                    goto the_end;
                }

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                /* Convert atempo's compressed output timeline back to the real source timeline. */
                if (graph_speed != 1.0 && !isnan(af->pts)) {
                    if (isnan(atempo_base_pts)) {
                        atempo_base_pts = af->pts;
                    }
                    af->pts = atempo_base_pts + graph_speed * (af->pts - atempo_base_pts);
                    af->duration *= graph_speed;
                }

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audioq.serial != is->auddec.pkt_serial) {
                    break;
                }
            }
            if (ret == AVERROR_EOF) {
                is->auddec.finished = is->auddec.pkt_serial;
            }
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);

    return ret;
}

static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    if (spdif.active) {
        return nb_samples;
    }

    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold * playback_speed) {
                    wanted_nb_samples = nb_samples +
                        (int)(diff * is->audio_src.freq / playback_speed);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
            }
        } else {
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

#define AUDIO_CATCHUP_SEEK_GAP 0.25
#define AUDIO_CATCHUP_START_GAP 2.0

static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused || is->start_pause_pending) {
        return -1;
    }

    for (;;) {
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if (is->abort_request || is->audioq.abort_request) {
                return -1;
            }
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2) {
                return -1;
            }
            av_usleep(1000);
        }
        if (!(af = frame_queue_peek_readable(&is->sampq))) {
            return -1;
        }
        frame_queue_next(&is->sampq);
        if (af->serial != is->audioq.serial) {
            continue;
        }
        if (af->serial == is->audio_catchup_serial &&
            !isnan(is->audio_catchup_pts) && !isnan(af->pts)) {
            if (is->audio_catchup_checked_serial != af->serial) {
                is->audio_catchup_checked_serial = af->serial;
                double gap = is->audio_catchup_pts - af->pts;
                double need = is->audio_catchup_startup ? AUDIO_CATCHUP_START_GAP
                                                        : AUDIO_CATCHUP_SEEK_GAP;
                if (gap <= need) {
                    is->audio_catchup_serial = -1;
                    break;
                }
            }
            if (af->pts + af->duration < is->audio_catchup_pts) {
                continue;
            }
            is->audio_catchup_serial = -1;
        }
        break;
    }

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                                  &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                  &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                                  0, NULL);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0) {
            return -1;
        }
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            /* clang-format off */
            if (swr_set_compensation(
                    is->swr_ctx,
                    (wanted_nb_samples - af->frame->nb_samples) *
                        is->audio_tgt.freq / af->frame->sample_rate,
                    wanted_nb_samples * is->audio_tgt.freq /
                        af->frame->sample_rate) < 0) {
                /* clang-format on */
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) {
            return AVERROR(ENOMEM);
        }
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            return -1;
        }
        if (len2 == out_count) {
            if (swr_init(is->swr_ctx) < 0) {
                swr_free(&is->swr_ctx);
            }
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    if (!isnan(af->pts)) {
        is->audio_clock = af->pts + af->duration;
    } else {
        is->audio_clock = NAN;
    }
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif

    return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);

static void SDLCALL sdl_audio_stream_callback(void *opaque, SDL_AudioStream *stream,
                                              int additional_amount, int total_amount av_unused) {
    if (additional_amount <= 0) {
        return;
    }
    av_fast_malloc(&audio_cb_buf, &audio_cb_buf_size, additional_amount);
    if (!audio_cb_buf) {
        return;
    }
    sdl_audio_callback(opaque, audio_cb_buf, additional_amount);
    SDL_PutAudioStreamData(stream, audio_cb_buf, additional_amount);
}

void audio_update_gain(VideoState *is) {
    float gain;

    if (!audio_stream_dev) {
        return;
    }
    if (spdif.active) {
        SDL_SetAudioStreamGain(audio_stream_dev, 1.0f);
        return;
    }
    gain = is->muted ? 0.0f : is->audio_volume / (float)FFP_MIX_MAXVOLUME;
    SDL_SetAudioStreamGain(audio_stream_dev, gain);
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = opaque;
    Uint8 *const stream_start = stream;
    const int len_total = len;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if ((unsigned int)is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* Just output silence upon error. */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        if (is->audio_buf) {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, 0, len1);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    if (spdif.active && is->muted) {
        memset(stream_start, 0, len_total);
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    if (!isnan(is->audio_clock) && is->audio_buf && !is->paused) {
        double cb_time = audio_callback_time / 1000000.0;
        double raw_pts = is->audio_clock -
            playback_speed *
                (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                is->audio_tgt.bytes_per_sec;
        double raw_drift = raw_pts - cb_time;
        double dt = cb_time - is->audclk_drift_time;
        double predicted = is->audclk_drift + (playback_speed - 1.0) * dt;
        if (!is->audclk_drift_valid ||
            is->audclk_drift_serial != is->audio_clock_serial ||
            is->audclk_drift_speed_serial != audio_speed_serial ||
            dt <= 0 || dt > 1.0 ||
            fabs(raw_drift - predicted) > AUDCLK_RESET_THRESHOLD) {
            is->audclk_drift = raw_drift;
            is->audclk_drift_valid = 1;
            is->audclk_drift_serial = is->audio_clock_serial;
            is->audclk_drift_speed_serial = audio_speed_serial;
        } else {
            is->audclk_drift = predicted + (raw_drift - predicted) * AUDCLK_SMOOTH_ALPHA;
        }
        is->audclk_drift_time = cb_time;
        set_clock_at(&is->audclk, is->audclk_drift + cb_time,
                     is->audio_clock_serial, cb_time);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    VideoState *is = opaque;
    SDL_AudioSpec wanted_spec;
    const char *env;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;
    int buffer_frames;
    int dev_frames = 0;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        return -1;
    }
    wanted_spec.format = SDL_AUDIO_S16;

    buffer_frames = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                          2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    if (!SDL_getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES")) {
        char frames_str[16];
        snprintf(frames_str, sizeof(frames_str), "%d", buffer_frames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_str);
    }

    if (audio_stream_dev) {
        SDL_DestroyAudioStream(audio_stream_dev);
        audio_stream_dev = NULL;
        audio_dev = 0;
    }
    audio_stream_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                 &wanted_spec,
                                                 sdl_audio_stream_callback, opaque);
    if (!audio_stream_dev) {
        return -1;
    }
    audio_dev = SDL_GetAudioStreamDevice(audio_stream_dev);
    audio_update_gain(is);

    {
        SDL_AudioSpec dev_spec;
        const char *drv = SDL_GetCurrentAudioDriver();
        media_info_note_audio_driver(drv, wanted_spec.channels,
                                     wanted_spec.freq);
        if (SDL_GetAudioDeviceFormat(audio_dev, &dev_spec, &dev_frames)) {
            audio_dev_spec = dev_spec;
            audio_dev_spec_valid = 1;
            media_info_note_audio_format((unsigned)dev_spec.format,
                                         dev_spec.channels, dev_spec.freq,
                                         dev_frames);
        } else {
            log_warn("SDL_GetAudioDeviceFormat failed: %s\n", SDL_GetError());
            dev_frames = 0;
            if (!av_sync_type_explicit) {
                is->av_sync_type = AV_SYNC_VIDEO_MASTER;
            }
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = wanted_spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0) {
        return -1;
    }
    /* clang-format off */
    audio_hw_params->frame_size =
        av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels,
                                   1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec =
        av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels,
                                   audio_hw_params->freq, audio_hw_params->fmt,
                                   1);
    /* clang-format on */
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        return -1;
    }
    if (dev_frames > 0) {
        return dev_frames * audio_hw_params->frame_size;
    }

    return buffer_frames * audio_hw_params->frame_size;
}

static int spdif_driver_can_passthrough(void) {
    const char *drv = SDL_GetCurrentAudioDriver();

    if (!drv) {
        return 0;
    }

    return !strcmp(drv, "alsa") || !strcmp(drv, "disk") || !strcmp(drv, "dummy");
}

static int spdif_device_takes_bits_verbatim(void) {
    int *chmap;
    int nb_chmap = 0;

    if (!audio_dev_spec_valid) {
        return 0;
    }

    if (!spdif_driver_can_passthrough()) {
        return 0;
    }

    if (audio_dev_spec.format != SDL_AUDIO_S16 ||
        audio_dev_spec.channels != spdif.channels ||
        audio_dev_spec.freq != spdif.rate) {
        return 0;
    }

    chmap = SDL_GetAudioDeviceChannelMap(audio_dev, &nb_chmap);
    if (chmap) {
        SDL_free(chmap);
        return 0;
    }

    return 1;
}

int audio_spdif_open(VideoState *is, AVStream *st, int *hw_buf_size) {
    AVChannelLayout layout = {0};
    int want_hd;
    int buf_size;
    int ret;

    spdif_warn_unknown_names();

    if (!spdif_selected(st->codecpar->codec_id, &want_hd)) {
        return 0;
    }

    spdif_close();
    if (!spdif_output_params(st->codecpar, want_hd)) {
        spdif_close();
        return 0;
    }
    if (spdif_muxer_open() < 0) {
        spdif_close();
        return 0;
    }

    if ((ret = av_channel_layout_copy(&layout, &spdif.ch_layout)) < 0) {
        spdif_close();
        return ret;
    }
    buf_size = audio_open(is, &layout, spdif.rate, &is->audio_tgt);
    av_channel_layout_uninit(&layout);
    if (buf_size < 0) {
        log_warn("Could not open the device for %dch @ %d Hz.\n",
                 spdif.channels, spdif.rate);
        audio_device_close();
        return 0;
    }

    if (is->audio_tgt.fmt != AV_SAMPLE_FMT_S16 ||
        is->audio_tgt.freq != spdif.rate ||
        av_channel_layout_compare(&is->audio_tgt.ch_layout, &spdif.ch_layout)) {
        audio_device_close();
        return 0;
    }
    if (!spdif_device_takes_bits_verbatim()) {
        if (!audio_spdif_force) {
            audio_device_close();
            return 0;
        }
    }

    spdif.active = 1;
    *hw_buf_size = buf_size;
    audio_update_gain(is);

    if (playback_speed != 1.0) {
        reanchor_clocks(is);
        playback_speed = 1.0;
        audio_speed_serial++;
    }

    if (is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        is->av_sync_type = AV_SYNC_AUDIO_MASTER;
    }
    media_info_note_audio_passthrough(avcodec_get_name(spdif.codec_id),
                                      spdif.dtshd_rate, spdif.channels,
                                      spdif.rate);

    return 1;
}

void toggle_mute(VideoState *is) {
    is->muted = !is->muted;
    global_muted = is->muted;
    audio_update_gain(is);
    osd_show_volume();
}

void update_volume(VideoState *is, int sign, double step) {
    int vol_max;

    if (spdif.active) {
        return;
    }

    vol_max = is->audio_volume_max > 0 ? is->audio_volume_max : FFP_MIX_MAXVOLUME;
    int max_pct = (int)lrint(100.0 * vol_max / FFP_MIX_MAXVOLUME);
    double cur_pct = lrint(100.0 * is->audio_volume / FFP_MIX_MAXVOLUME);
    if (cur_pct > max_pct) {
        cur_pct = max_pct;
    }
    double target_pct = sign > 0 ? (floor(cur_pct / step + 1e-6) + 1) * step
                                 : (ceil(cur_pct / step - 1e-6) - 1) * step;
    if (target_pct < 0) {
        target_pct = 0;
    }
    if (target_pct > max_pct) {
        target_pct = max_pct;
    }
    is->audio_volume = av_clip((int)lrint(FFP_MIX_MAXVOLUME * target_pct / 100.0), 0, vol_max);
    audio_update_gain(is);
    osd_show_volume();
    is->force_refresh = 1;
}

void audio_device_resume(void) {
    SDL_ResumeAudioDevice(audio_dev);
}

void audio_device_close(void) {
    SDL_DestroyAudioStream(audio_stream_dev);
    audio_stream_dev = NULL;
    audio_dev = 0;
    audio_dev_spec_valid = 0;
    av_freep(&audio_cb_buf);
    audio_cb_buf_size = 0;
    spdif_close();
}
