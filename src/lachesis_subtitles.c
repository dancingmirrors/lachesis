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

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include <SDL3/SDL.h>

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_subtitles.h"

int subtitle_thread(void *arg) {
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq))) {
            return 0;
        }

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0) {
            break;
        }

        pts = 0;

        if (got_subtitle) {
            if (sp->sub.pts != AV_NOPTS_VALUE) {
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            }
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* Now we can update the picture count. */
            frame_queue_push(&is->subpq);
        }
    }

    return 0;
}

static int sub_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request || is->sub_seek_pending;
}

static int sub_read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = is->sub_ic;
    AVPacket *pkt = av_packet_alloc();
    int sent_eof = 0;

    if (!pkt) {
        return AVERROR(ENOMEM);
    }

    for (;;) {
        if (is->abort_request) {
            break;
        }

        if (is->sub_seek_pending) {
            avformat_seek_file(ic, -1,
                               is->sub_seek_min,
                               is->sub_seek_pos,
                               is->sub_seek_max,
                               is->sub_seek_flags);
            is->sub_seek_pending = 0;
            sent_eof = 0;
            continue;
        }

        if (!sent_eof &&
            stream_has_enough_packets(is->subtitle_st, is->sub_ext_stream, &is->subtitleq)) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                sent_eof = 1;
                SDL_Delay(50);
            } else if (!is->sub_seek_pending && !is->abort_request) {
                SDL_Delay(10);
            }
            continue;
        }

        if (sent_eof) {
            sent_eof = 0;
        }

        if (pkt->stream_index == is->sub_ext_stream) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);

    return 0;
}

int open_external_subtitle(VideoState *is) {
    static const char *const sub_exts[] = {
        "srt", "ass", "ssa", "vtt", "sub", NULL};

    if (!is->filename || is->subtitle_st || subtitle_disable) {
        return -1;
    }
    /* XXX */
    if (is->archive_path || strstr(is->filename, "://")) {
        return -1;
    }

    const char *dot = strrchr(is->filename, '.');
    const char *slash = strrchr(is->filename, '/');
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    size_t base_len = dot ? (size_t)(dot - is->filename) : strlen(is->filename);

    char path[4096];
    int found = 0;
    for (int i = 0; sub_exts[i]; i++) {
        snprintf(path, sizeof(path), "%.*s.%s", (int)base_len, is->filename, sub_exts[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }

    AVFormatContext *sic = avformat_alloc_context();
    if (!sic) {
        return AVERROR(ENOMEM);
    }
    sic->interrupt_callback.callback = sub_interrupt_cb;
    sic->interrupt_callback.opaque = is;

    int idx;
    AVStream *st;
    const AVCodec *codec;
    AVCodecContext *avctx;

    int ret = avformat_open_input(&sic, path, NULL, NULL);
    if (ret < 0) {
        log_warn("Could not open external subtitle '%s'!\n", path);
        return ret;
    }
    if ((ret = avformat_find_stream_info(sic, NULL)) < 0) {
        goto fail;
    }

    idx = av_find_best_stream(sic, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (idx < 0) {
        ret = idx;
        goto fail;
    }

    st = sic->streams[idx];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (subtitle_codec_name) {
        codec = avcodec_find_decoder_by_name(subtitle_codec_name);
    }
    if (!codec) {
        ret = AVERROR_DECODER_NOT_FOUND;
        goto fail;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = avcodec_parameters_to_context(avctx, st->codecpar);
    if (ret < 0) {
        avcodec_free_context(&avctx);
        goto fail;
    }
    avctx->pkt_timebase = st->time_base;
    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        avcodec_free_context(&avctx);
        goto fail;
    }

    if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0) {
        avcodec_free_context(&avctx);
        goto fail;
    }

    is->sub_ic = sic;
    is->sub_ext_stream = idx;
    is->subtitle_st = st;

    if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0) {
        decoder_destroy(&is->subdec);
        is->subtitle_st = NULL;
        is->sub_ic = NULL;
        goto fail;
    }

    is->sub_read_tid = SDL_CreateThread(sub_read_thread, "sub_reader", is);
    if (!is->sub_read_tid) {
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        is->subtitle_st = NULL;
        is->sub_ic = NULL;
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    log_info("Loaded external subtitle: %s\n", path);
    return 0;

fail:
    avformat_close_input(&sic);
    return ret;
}
