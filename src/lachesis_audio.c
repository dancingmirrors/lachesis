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

#include "lachesis_audio.h"
#include "lachesis_cmdutils.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"

static SDL_AudioDeviceID audio_dev;
static SDL_AudioStream *audio_stream_dev;

static uint8_t *audio_cb_buf;
static unsigned int audio_cb_buf_size;

int64_t audio_callback_time;
int audio_speed_serial = 0;

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                                 enum AVSampleFormat fmt2, int64_t channel_count2) {
    /* If channel count == 1, planar and non-planar formats are the same. */
    if (channel_count1 == 1 && channel_count2 == 1) {
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    } else {
        return channel_count1 != channel_count2 || fmt1 != fmt2;
    }
}

static void append_atempo_chain(AVBPrint *bp, double speed) {
    int first = 1;
    while (speed > 2.0 + 1e-9) {
        av_bprintf(bp, "%satempo=2.0", first ? "" : ",");
        speed /= 2.0;
        first = 0;
    }
    while (speed < 0.5 - 1e-9) {
        av_bprintf(bp, "%satempo=0.5", first ? "" : ",");
        speed *= 2.0;
        first = 0;
    }
    av_bprintf(bp, "%satempo=%.6g", first ? "" : ",", speed);
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
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc())) {
        return AVERROR(ENOMEM);
    }
    is->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e))) {
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    }
    if (strlen(aresample_swr_opts)) {
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    }
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

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

int audio_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int last_speed_serial = audio_speed_serial;
    double atempo_base_pts = NAN;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0) {
            goto the_end;
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
                if (playback_speed != 1.0 && !isnan(af->pts)) {
                    if (isnan(atempo_base_pts)) {
                        atempo_base_pts = af->pts;
                    }
                    af->pts = atempo_base_pts + playback_speed * (af->pts - atempo_base_pts);
                    af->duration *= playback_speed;
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

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
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

static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused || is->start_pause_pending) {
        return -1;
    }

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2) {
                return -1;
            }
            av_usleep(1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq))) {
            return -1;
        }
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

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
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
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

/* SDL_MixAudio() multiplies without saturating. */
static void audio_amplify_s16(uint8_t *dst, const uint8_t *src, int len, float gain) {
    int16_t *d = (int16_t *)dst;
    const int16_t *s = (const int16_t *)src;
    int n = len / (int)sizeof(int16_t);
    for (int i = 0; i < n; i++) {
        d[i] = (int16_t)av_clip_int16((int)lrintf(s[i] * gain));
    }
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = opaque;
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
        if (!is->muted && is->audio_buf && is->audio_volume == FFP_MIX_MAXVOLUME) {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        } else if (!is->muted && is->audio_buf && is->audio_volume > FFP_MIX_MAXVOLUME) {
            audio_amplify_s16(stream,
                              (uint8_t *)is->audio_buf + is->audio_buf_index,
                              len1, is->audio_volume / (float)FFP_MIX_MAXVOLUME);
        } else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf) {
                /* clang-format off */
                SDL_MixAudio(stream,
                             (uint8_t *)is->audio_buf + is->audio_buf_index,
                             SDL_AUDIO_S16, len1,
                             is->audio_volume / (float)FFP_MIX_MAXVOLUME);
                /* clang-format on */
            }
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        /* clang-format off */
        set_clock_at(&is->audclk,
                     is->audio_clock -
                         (double)(2 * is->audio_hw_buf_size +
                                  is->audio_write_buf_size) /
                             is->audio_tgt.bytes_per_sec,
                     is->audio_clock_serial,
                     audio_callback_time / 1000000.0);
        /* clang-format on */
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

    audio_stream_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                 &wanted_spec,
                                                 sdl_audio_stream_callback, opaque);
    if (!audio_stream_dev) {
        return -1;
    }
    audio_dev = SDL_GetAudioStreamDevice(audio_stream_dev);

    {
        SDL_AudioSpec dev_spec;
        const char *drv = SDL_GetCurrentAudioDriver();
        media_info_note_audio_driver(drv, wanted_spec.channels,
                                     wanted_spec.freq);
        if (SDL_GetAudioDeviceFormat(audio_dev, &dev_spec, &dev_frames)) {
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

void toggle_mute(VideoState *is) {
    is->muted = !is->muted;
    global_muted = is->muted;
    osd_show_volume();
}

void update_volume(VideoState *is, int sign, double step) {
    int vol_max = is->audio_volume_max > 0 ? is->audio_volume_max : FFP_MIX_MAXVOLUME;
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
    av_freep(&audio_cb_buf);
    audio_cb_buf_size = 0;
}
