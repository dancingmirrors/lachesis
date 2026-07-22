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

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include <SDL3/SDL.h>

#include "lachesis_archive.h"
#include "lachesis_audio.h"
#include "lachesis_demux.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_network.h"
#include "lachesis_options.h"
#include "lachesis_subtitles.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

static double read_ahead_secs = 1.0;
static int64_t max_queue_bytes = MAX_QUEUE_SIZE;

AVDictionary *format_opts;

static void print_error(const char *filename, int err) {
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, av_err2str(err));
}

/* Fail if any option in m was left unconsumed by the libav* call it was
 * passed to. */
static int check_avoptions(AVDictionary *m) {
    const AVDictionaryEntry *t = av_dict_iterate(m, NULL);
    if (t) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }

    return 0;
}

static int try_hwaccel(AVBufferRef **device_ctx, const char *name) {
    enum AVHWDeviceType type;
    int ret;
    AVBufferRef *vk_dev;

    type = av_hwdevice_find_type_by_name(name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return AVERROR(ENOTSUP);
    }

    if (vk_renderer) {
        ret = vk_renderer_get_hw_dev(vk_renderer, &vk_dev);
        if (ret >= 0) {
            ret = av_hwdevice_ctx_create_derived(device_ctx, type, vk_dev, 0);
            if (!ret) {
                return 0;
            }
            if (ret != AVERROR(ENOSYS)) {
                return ret;
            }
        }
    }

    return av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0);
}

static int create_hwaccel(AVBufferRef **device_ctx) {
    static const char *auto_hwaccels_vk[] = {
        "vulkan", "vaapi", "videotoolbox", "d3d11va", "dxva2", NULL};
    static const char *auto_hwaccels_sw[] = {
        "vaapi", "videotoolbox", "d3d11va", "dxva2", NULL};
    const char *const *auto_hwaccels = vk_renderer ? auto_hwaccels_vk
                                                   : auto_hwaccels_sw;
    int ret;

    *device_ctx = NULL;

    if (no_hwaccel) {
        return 0;
    }

    if (hwaccel) {
        ret = try_hwaccel(device_ctx, hwaccel);
        if (ret < 0 && ret != AVERROR(ENOSYS)) {
            log_dead("hwaccel %s is not available!\n", hwaccel);
        }
        if (ret >= 0) {
            media_info_set_hwaccel(hwaccel);
        }
        return ret < 0 ? ret : 0;
    }

    for (int i = 0; auto_hwaccels[i]; i++) {
        ret = try_hwaccel(device_ctx, auto_hwaccels[i]);
        if (!ret) {
            media_info_set_hwaccel(auto_hwaccels[i]);
            return 0;
        }
        *device_ctx = NULL;
    }

    return 0;
}

static int format_lacks_timestamps(const AVFormatContext *ic) {
    if (!ic || !ic->iformat) {
        return 0;
    }
    if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        return 1;
    }
    const char *name = ic->iformat->name;
    if (!strcmp(name, "h264") || !strcmp(name, "hevc") || !strcmp(name, "vvc")) {
        return 1;
    }

    return 0;
}

static int is_http_input(const char *fn) {
    return fn && (!strncmp(fn, "http://", 7) || !strncmp(fn, "https://", 8) || !strncmp(fn, "ytdl://", 7));
}

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = {0};
    int ret = 0;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) {
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->last_audio_stream = stream_index;
        forced_codec_name = audio_codec_name;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->last_subtitle_stream = stream_index;
        forced_codec_name = subtitle_codec_name;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->last_video_stream = stream_index;
        forced_codec_name = video_codec_name;
        break;
    default:
        break;
    }
    if (forced_codec_name) {
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    }
    if (!codec) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(&avctx->hw_device_ctx);
        if (ret < 0) {
            goto fail;
        }
    }

    if (avctx->hw_device_ctx && avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(62, 11, 100)
        av_dict_set(&opts, "threads", "1", 0);
#else
        av_dict_set(&opts, "threads", "auto", 0);
#endif
    }

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    ret = check_avoptions(opts);
    if (ret < 0) {
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
        AVFilterContext *sink;

        is->audio_filter_src.freq = avctx->sample_rate;
        ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
        if (ret < 0) {
            goto fail;
        }
        is->audio_filter_src.fmt = avctx->sample_fmt;
        if ((ret = configure_audio_filters(is, afilters_opt, 0)) < 0) {
            goto fail;
        }
        sink = is->out_audio_filter;
        sample_rate = av_buffersink_get_sample_rate(sink);
        ret = av_buffersink_get_ch_layout(sink, &ch_layout);
        if (ret < 0) {
            goto fail;
        }
    }

        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0) {
            goto fail;
        }
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;

        is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if (format_lacks_timestamps(is->ic)) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0) {
            goto out;
        }
        audio_device_resume();
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if (is->decode_degraded) {
            apply_degraded_decode(avctx);
        }

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if (format_lacks_timestamps(is->ic)) {
            is->viddec.start_pts = is->video_st->start_time;
            is->viddec.start_pts_tb = is->video_st->time_base;
        }
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0) {
            goto out;
        }
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0) {
            goto fail;
        }
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0) {
            goto out;
        }
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request;
}

int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
        queue->abort_request ||
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        ((queue->nb_packets > MIN_FRAMES && (!queue->duration)) || (av_q2d(st->time_base) * queue->duration > read_ahead_secs));
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }

    return 0;
}

static int detect_still_image(const AVFormatContext *ic) {
    if (!ic || !ic->iformat) {
        return 0;
    }
    const char *name = ic->iformat->name;
    static const char *const still_formats[] = {
        "image2",
        "png_pipe", "jpeg_pipe", "mjpeg_pipe", "bmp_pipe",
        "tiff_pipe", "tga_pipe", "dpx_pipe", "exr_pipe",
        "pnm_pipe", "sgi_pipe", "xwd_pipe", "xbm_pipe",
        NULL};
    for (int i = 0; still_formats[i]; i++) {
        if (!strcmp(name, still_formats[i])) {
            return 1;
        }
    }
    /* There might be an additional edge case to consider here. */
    if (!strcmp(name, "mjpeg")) {
        for (unsigned int i = 0; i < ic->nb_streams; i++) {
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                return 0;
            }
        }
        return 1;
    }
    if (!strcmp(name, "gif") || !strcmp(name, "webp_pipe")) {
        for (unsigned int i = 0; i < ic->nb_streams; i++) {
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return ic->streams[i]->nb_frames == 1;
            }
        }
    }

    return 0;
}

static const AVInputFormat *guess_archive_entry_format(const char *entry_name) {
    const char *dot = strrchr(entry_name, '.');
    if (!dot) {
        return NULL;
    }
    const char *ext = dot + 1;
    struct {
        const char *ext;
        const char *fmt;
    } map[] = {
        {"jpg", "mjpeg"},
        {"jpeg", "mjpeg"},
        {"png", "png_pipe"},
        {"bmp", "bmp_pipe"},
        {"gif", "gif"},
        {"webp", "webp_pipe"},
        {"tiff", "tiff_pipe"},
        {"tif", "tiff_pipe"},
        {"tga", "tga_pipe"},
        {NULL, NULL},
    };
    for (int i = 0; map[i].ext; i++) {
        if (!strcasecmp(ext, map[i].ext)) {
            return av_find_input_format(map[i].fmt);
        }
    }

    return NULL;
}

static int audio_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request || is->audio_seek_pending;
}

static int audio_read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = is->audio_ic;
    AVPacket *pkt = av_packet_alloc();
    int sent_eof = 0;

    if (!pkt) {
        return AVERROR(ENOMEM);
    }

    for (;;) {
        if (is->abort_request) {
            break;
        }

        if (is->audio_seek_pending) {
            avformat_seek_file(ic, -1,
                               is->audio_seek_min,
                               is->audio_seek_pos,
                               is->audio_seek_max,
                               is->audio_seek_flags);
            is->audio_seek_pending = 0;
            sent_eof = 0;
            continue;
        }

        if (!sent_eof && !is->realtime &&
            stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq)) {
            SDL_Delay(10);
            continue;
        }
        int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                if (!sent_eof) {
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                    sent_eof = 1;
                }
                SDL_Delay(50);
            } else if (!is->audio_seek_pending && !is->abort_request) {
                SDL_Delay(10);
            }
            continue;
        }

        if (sent_eof) {
            packet_queue_flush(&is->audioq);
            sent_eof = 0;
        }

        if (pkt->stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);

    return 0;
}

/* This thread gets the stream from disk or the network. */
int read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    const AVDictionaryEntry *t;
    SDL_Mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int extension_picky_set = 0;
    int64_t pkt_ts;
    int ff_quit_reason = FF_QUIT_REASON_ERROR;

    if (!wait_mutex) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if (is_http_input(is->filename) &&
        !av_dict_get(format_opts, "extension_picky", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "extension_picky", "0", AV_DICT_DONT_OVERWRITE);
        extension_picky_set = 1;
    }
    if (is->archive_path && is->entry_name) {
        is->archive_avio = archive_entry_open_avio(is->archive_path, is->entry_name);
        if (!is->archive_avio) {
            log_warn("Could not open archive entry '%s' in '%s'!\n",
                     is->entry_name, is->archive_path);
            ret = -1;
            goto fail;
        }
        ic->pb = is->archive_avio;
        ic->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    if (!is->archive_path && !ytdl_disable) {
        const char *fn = is->filename;
        if (strncmp(fn, "http://", 7) == 0 || strncmp(fn, "https://", 8) == 0 ||
            strncmp(fn, "ytdl://", 7) == 0) {
            if (strncmp(fn, "ytdl://", 7) == 0) {
                char *bare = av_strdup(fn + 7);
                if (bare) {
                    av_free(is->filename);
                    is->filename = bare;
                    fn = is->filename;
                }
            }
            char *vurl = NULL, *aurl = NULL;
            if (ytdl_resolve(fn, &vurl, &aurl) > 0) {
                is->ytdl_source_url = av_strdup(fn);
                av_free(is->filename);
                is->filename = vurl;
                is->ytdl_audio_url = aurl;
                is->ytdl_vio = ytdl_chunked_create(is->filename, is);
                if (is->ytdl_vio) {
                    ic->pb = ytdl_chunked_pb(is->ytdl_vio);
                    ic->flags |= AVFMT_FLAG_CUSTOM_IO;
                } else {
                    set_ytdl_http_opts(&format_opts);
                }
            } else {
                log_warn("yt-dlp failed, so trying direct open.\n");
            }
        }
    }
    {
        const char *open_url = (is->archive_path && is->entry_name)
            ? is->entry_name
            : is->filename;
        const AVInputFormat *open_fmt = is->iformat;
        if (!open_fmt && is->archive_avio && is->entry_name) {
            open_fmt = guess_archive_entry_format(is->entry_name);
        }
        err = avformat_open_input(&ic, open_url, open_fmt, &format_opts);
    }
    if (scan_all_pmts_set) {
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    }
    if (extension_picky_set) {
        av_dict_set(&format_opts, "extension_picky", NULL, AV_DICT_MATCH_CASE);
    }
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    ret = check_avoptions(format_opts);
    if (ret < 0) {
        goto fail;
    }
    is->ic = ic;

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        if (!is->archive_avio) {
            ret = -1;
            goto fail;
        }
    }

    is->is_still_image = detect_still_image(ic);

    if (ic->pb) {
        ic->pb->eof_reached = 0;
        if (is->archive_avio && is->is_still_image) {
            avio_seek(ic->pb, 0, SEEK_SET);
        }
    }

    if (seek_by_bytes < 0) {
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
            !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
            strcmp("ogg", ic->iformat->name);
    }

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0))) {
        if (playlist_size > 1) {
            window_title = av_asprintf("%s - %s [%d/%d]", program_name,
                                       t->value, playlist_pos + 1, playlist_size);
        } else {
            window_title = av_asprintf("%s - %s", program_name, t->value);
        }
    }

    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        if (ic->start_time != AV_NOPTS_VALUE) {
            timestamp += ic->start_time;
        }
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    {
        int network = is->realtime || is->ytdl_source_url ||
            (is->filename &&
             (!strncmp(is->filename, "http://", 7) ||
              !strncmp(is->filename, "https://", 8)));
        read_ahead_secs = opt_cache_secs > 0.0f
            ? opt_cache_secs
            : (network ? 30.0 : 1.0);
        max_queue_bytes = opt_cache_size_mb > 0
            ? (int64_t)opt_cache_size_mb * 1024 * 1024
            : (network ? (int64_t)128 * 1024 * 1024
                       : (int64_t)MAX_QUEUE_SIZE);
    }

    for (i = 0; i < (int)ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1) {
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0) {
                st_index[type] = i;
            }
        }
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
    ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable) {
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    }
    if (!audio_disable) {
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    }
    if (!video_disable && !subtitle_disable) {
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width) {
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }

    if ((start_paused || is->begin_paused) && !is->is_still_image) {
        is->start_pause_pending = 1;
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
        if (ret < 0 && hwaccel && !no_hwaccel) {
            fatal_error_pending = 1;
            goto fail;
        }
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (!is->subtitle_st) {
        open_external_subtitle(is);
    }

    print_stream_info(is);

    if (is->ytdl_audio_url && !audio_disable && is->audio_stream < 0) {
        AVFormatContext *aic = avformat_alloc_context();
        if (aic) {
            aic->interrupt_callback.callback = audio_interrupt_cb;
            aic->interrupt_callback.opaque = is;
            int audio_open_ret;
            is->ytdl_aio = ytdl_chunked_create(is->ytdl_audio_url, is);
            if (is->ytdl_aio) {
                aic->pb = ytdl_chunked_pb(is->ytdl_aio);
                aic->flags |= AVFMT_FLAG_CUSTOM_IO;
                audio_open_ret = avformat_open_input(&aic, is->ytdl_audio_url, NULL, NULL);
            } else {
                AVDictionary *audio_opts = NULL;
                set_ytdl_http_opts(&audio_opts);
                audio_open_ret = avformat_open_input(&aic, is->ytdl_audio_url, NULL, &audio_opts);
                av_dict_free(&audio_opts);
            }
            if (audio_open_ret >= 0 &&
                avformat_find_stream_info(aic, NULL) >= 0) {
                int aidx = av_find_best_stream(aic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
                if (aidx >= 0) {
                    is->audio_ic = aic;
                    AVFormatContext *save_ic = is->ic;
                    is->ic = aic;
                    stream_component_open(is, aidx);
                    is->ic = save_ic;
                    is->audio_read_tid = SDL_CreateThread(audio_read_thread,
                                                          "audio_read", is);
                    if (!is->audio_read_tid) {
                        avformat_close_input(&is->audio_ic);
                        ytdl_chunked_free(&is->ytdl_aio);
                    }
                } else {
                    avformat_close_input(&aic);
                    ytdl_chunked_free(&is->ytdl_aio);
                }
            } else {
                avformat_close_input(&aic);
                ytdl_chunked_free(&is->ytdl_aio);
            }
        }
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        ret = -1;
        goto fail;
    }

    if ((start_paused || is->begin_paused) && !is->is_still_image) {
        if (is->video_stream >= 0) {
            is->step = 1;
        } else {
            is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = 1;
        }
    }

    for (;;) {
        if (is->abort_request) {
            break;
        }
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) {
                is->read_pause_return = av_read_pause(ic);
            } else {
                av_read_play(ic);
            }
        }
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            /* XXX */
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
                if (is->audio_ic) {
                    is->audio_seek_min = seek_min;
                    is->audio_seek_pos = seek_target;
                    is->audio_seek_max = seek_max;
                    is->audio_seek_flags = is->seek_flags;
                    is->audio_seek_pending = 1;
                }
                if (is->sub_ic) {
                    packet_queue_flush(&is->subtitleq);
                    is->sub_seek_min = seek_min;
                    is->sub_seek_pos = seek_target;
                    is->sub_seek_max = seek_max;
                    is->sub_seek_flags = is->seek_flags;
                    is->sub_seek_pending = 1;
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused) {
                step_to_next_frame(is);
            }
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0) {
                    goto fail;
                }
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        if (!is->realtime &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > max_queue_bytes ||
             (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
              stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
              stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            SDL_LockMutex(wait_mutex);
            SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (is->is_still_image) {
                is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = 1;
            } else if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (is->ytdl_source_url && ic->pb && avio_size(ic->pb) <= 0) {
                /* An unknown size might be a live stream. */
                SDL_LockMutex(wait_mutex);
                SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 100);
                SDL_UnlockMutex(wait_mutex);
                continue;
            } else if (keep_open && playlist_pos + 1 >= playlist_size) {
                is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = 1;
                SDL_LockMutex(wait_mutex);
                SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 100);
                SDL_UnlockMutex(wait_mutex);
                continue;
            } else {
                ff_quit_reason = FF_QUIT_REASON_EOF;
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0) {
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                }
                if (is->audio_stream >= 0) {
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                }
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                goto fail;
            }
            SDL_LockMutex(wait_mutex);
            SDL_WaitConditionTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }

        ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        ic->streams[pkt->stream_index]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = play_duration == AV_NOPTS_VALUE ||
            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(ic->streams[pkt->stream_index]->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000 <=
                ((double)play_duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range && !is->audio_ic) {
            packet_queue_put(&is->audioq, pkt);
            /* clang-format off */
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
                   !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            /* clang-format on */
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
        /* ic->pb (archive_avio) is not freed by avformat_close_input with
         * AVFMT_FLAG_CUSTOM_IO, so free it now since is->ic was never set.
         */
        archive_entry_close_avio(is->archive_avio);
        is->archive_avio = NULL;
    }
    /* If is->ic was set, stream_close() will call avformat_close_input and
     * then archive_entry_close_avio via is->archive_avio.
     */
    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.code = ff_quit_reason;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);

    return 0;
}
