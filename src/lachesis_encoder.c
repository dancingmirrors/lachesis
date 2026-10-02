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

#include "lachesis_config.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_aspect.h"
#include "lachesis_demux.h"
#include "lachesis_encoder.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_seek.h"

#define ENCODE_VIDEO_TB ((AVRational){1, 60000})
#define ENCODE_FALLBACK_FRAME_DURATION 0.04
#define ENCODE_X264_CRF "15"
#define ENCODE_MPEG4_QSCALE 3
#define ENCODE_AUDIO_BITRATE_PER_CHANNEL 96000
#define ENCODE_AUDIO_BITRATE_RATE 48000
#define ENCODE_AUDIO_BITRATE_MAX 512000
#define ENCODE_SILENCE_CHUNK 4096
#define ENCODE_SYNC_MAX AV_NOSYNC_THRESHOLD
#define ENCODE_STRAY_AHEAD 1.0
#define ENCODE_HOLD_RESEND 1.0
#define ENCODE_INTERLEAVE_MAX 60
#define ENCODE_PACE_SNAP 0.002
#define ENCODE_AUDIO_JUMP 0.1
#define ENCODE_WAIT_MS 5
#define ENCODE_PROGRESS_US 250000

static const struct {
    const char *ext;
    const char *format;
} output_formats[] = {
    {"mp4", "mp4"},
    {"m4v", "mp4"},
    {"mkv", "matroska"},
};

typedef struct Encoder {
    const AVOutputFormat *oformat;
    const AVCodec *vcodec;
    const AVCodec *acodec;
    enum AVPixelFormat pix_fmts[2];
    AVDictionary *metadata;

    AVFormatContext *oc;
    AVPacket *pkt;
    int want_video;
    int want_audio;
    int started;
    int has_video;
    int has_audio;
    double origin;
    double hold_max;

    AVCodecContext *venc;
    AVStream *vst;
    struct SwsContext *sws;
    Renderer *renderer;
    AVFrame *pending;
    int64_t pending_ts;
    int pending_resent;
    int64_t v_frames;
    int64_t v_dropped;
    int64_t v_strays;
    double v_shift;
    double v_last_src;
    double v_last_out;
    double v_last_dur;
    int v_free_running;
    int v_rescale_warned;

    AVCodecContext *aenc;
    AVStream *ast;
    AVAudioFifo *fifo;
    AVFrame *silence;
    int64_t a_samples;
    int64_t a_sent;
    double a_next_src;
    int a_started;
    int a_jumps;
    int a_mismatch_warned;

    double anchor_src;
    double anchor_out;
    int anchor_valid;

    int64_t start_us;
    int64_t progress_us;
} Encoder;

static Encoder enc = {
    .pix_fmts = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
};

static const char *path_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');

    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
#endif

    if (!dot || (slash && dot < slash)) {
        return NULL;
    }

    return dot + 1;
}

const AVOutputFormat *encoder_output_format(const char *path) {
    const char *ext = path_extension(path);

    if (!ext) {
        return NULL;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(output_formats); i++) {
        if (!av_strcasecmp(ext, output_formats[i].ext)) {
            return av_guess_format(output_formats[i].format, NULL, NULL);
        }
    }

    return NULL;
}

int encoder_enabled(void) {
    return output_filename != NULL;
}

static enum AVPixelFormat pick_pix_fmt(const AVCodec *codec) {
    const void *configs = NULL;
    const enum AVPixelFormat *fmts;
    int count = 0;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     &configs, &count) < 0 ||
        !configs || count <= 0) {
        return AV_PIX_FMT_YUV420P;
    }
    fmts = configs;
    for (int i = 0; i < count; i++) {
        if (fmts[i] == AV_PIX_FMT_YUV420P) {
            return AV_PIX_FMT_YUV420P;
        }
    }

    return fmts[0];
}

int encoder_init(void) {
    static const char *const video_encoders[] = {"libx264", "mpeg4"};

    enc.oformat = encoder_output_format(output_filename);
    if (!enc.oformat) {
        return AVERROR_MUXER_NOT_FOUND;
    }
    if (!video_disable) {
        for (size_t i = 0; !enc.vcodec && i < FF_ARRAY_ELEMS(video_encoders); i++) {
            enc.vcodec = avcodec_find_encoder_by_name(video_encoders[i]);
        }
        if (enc.vcodec) {
            enc.pix_fmts[0] = pick_pix_fmt(enc.vcodec);
        } else {
            log_warn("This FFmpeg has neither libx264 nor the MPEG-4 encoder, "
                     "so the output gets no video.\n");
            video_disable = 1;
        }
    }
    if (!audio_disable) {
        enc.acodec = avcodec_find_encoder_by_name("aac");
        if (!enc.acodec) {
            enc.acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        }
        if (!enc.acodec) {
            log_warn("This FFmpeg has no AAC encoder, so the output gets no audio.\n");
            audio_disable = 1;
        }
    }
    if (!enc.vcodec && !enc.acodec) {
        return AVERROR(EINVAL);
    }
    enc.pkt = av_packet_alloc();
    if (!enc.pkt) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

const enum AVPixelFormat *encoder_pix_fmts(int *count) {
    *count = 1;

    return enc.pix_fmts;
}

void encoder_note_input(const AVFormatContext *ic) {
    av_dict_free(&enc.metadata);
    av_dict_copy(&enc.metadata, ic->metadata, 0);
}

static int pick_sample_rate(const AVCodec *codec, int want) {
    const void *configs = NULL;
    const int *rates;
    int count = 0;
    int above = 0;
    int highest = 0;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                     &configs, &count) < 0 ||
        !configs || count <= 0) {
        return want;
    }
    rates = configs;
    for (int i = 0; i < count; i++) {
        if (rates[i] == want) {
            return want;
        }
        if (rates[i] > want && (!above || rates[i] < above)) {
            above = rates[i];
        }
        highest = FFMAX(highest, rates[i]);
    }

    return above ? above : highest;
}

static enum AVSampleFormat pick_sample_fmt(const AVCodec *codec) {
    const void *configs = NULL;
    const enum AVSampleFormat *fmts;
    int count = 0;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     &configs, &count) < 0 ||
        !configs || count <= 0) {
        return AV_SAMPLE_FMT_FLTP;
    }
    fmts = configs;
    for (int i = 0; i < count; i++) {
        if (fmts[i] == AV_SAMPLE_FMT_FLTP) {
            return AV_SAMPLE_FMT_FLTP;
        }
    }

    return fmts[0];
}

static AVCodecContext *audio_encoder_try(const AVChannelLayout *layout,
                                         int sample_rate,
                                         enum AVSampleFormat fmt) {
    AVCodecContext *avctx = avcodec_alloc_context3(enc.acodec);

    if (!avctx) {
        return NULL;
    }
    avctx->sample_rate = sample_rate;
    avctx->sample_fmt = fmt;
    avctx->time_base = (AVRational){1, sample_rate};
    avctx->bit_rate = FFMIN(av_rescale((int64_t)ENCODE_AUDIO_BITRATE_PER_CHANNEL *
                                           layout->nb_channels,
                                       sample_rate, ENCODE_AUDIO_BITRATE_RATE),
                            ENCODE_AUDIO_BITRATE_MAX);
    if (enc.oformat->flags & AVFMT_GLOBALHEADER) {
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (av_channel_layout_copy(&avctx->ch_layout, layout) < 0 ||
        avcodec_open2(avctx, enc.acodec, NULL) < 0) {
        avcodec_free_context(&avctx);
    }

    return avctx;
}

int encoder_open_audio(const AVChannelLayout *layout, int sample_rate,
                       struct AudioParams *tgt) {
    AVChannelLayout tries[3] = {{0}};
    int nb_tries = 0;
    enum AVSampleFormat fmt;
    char from[64], to[64];
    int rate;
    int ret;

    if (!enc.acodec || sample_rate <= 0) {
        return AVERROR(EINVAL);
    }
    avcodec_free_context(&enc.aenc);
    rate = pick_sample_rate(enc.acodec, sample_rate);
    fmt = pick_sample_fmt(enc.acodec);

    if (layout->order == AV_CHANNEL_ORDER_NATIVE && av_channel_layout_check(layout) &&
        av_channel_layout_copy(&tries[nb_tries], layout) >= 0) {
        nb_tries++;
    }
    if (layout->nb_channels > 0) {
        av_channel_layout_default(&tries[nb_tries++], layout->nb_channels);
    }
    tries[nb_tries++] = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;

    for (int i = 0; i < nb_tries && !enc.aenc; i++) {
        if (i && !av_channel_layout_compare(&tries[i], &tries[i - 1])) {
            continue;
        }
        enc.aenc = audio_encoder_try(&tries[i], rate, fmt);
    }
    for (int i = 0; i < nb_tries; i++) {
        av_channel_layout_uninit(&tries[i]);
    }
    if (!enc.aenc) {
        log_warn("Could not open the %s encoder for %d Hz audio.\n",
                 enc.acodec->name, rate);
        return AVERROR(EINVAL);
    }

    if (enc.aenc->ch_layout.nb_channels != layout->nb_channels) {
        av_channel_layout_describe(layout, from, sizeof(from));
        av_channel_layout_describe(&enc.aenc->ch_layout, to, sizeof(to));
        log_warn("The %s encoder does not take %s audio, so mixing it to %s.\n",
                 enc.acodec->name, from, to);
    }

    tgt->fmt = enc.aenc->sample_fmt;
    tgt->freq = enc.aenc->sample_rate;
    av_channel_layout_uninit(&tgt->ch_layout);
    if ((ret = av_channel_layout_copy(&tgt->ch_layout, &enc.aenc->ch_layout)) < 0) {
        return ret;
    }
    tgt->frame_size = av_samples_get_buffer_size(NULL, tgt->ch_layout.nb_channels,
                                                 1, tgt->fmt, 1);
    tgt->bytes_per_sec = av_samples_get_buffer_size(NULL, tgt->ch_layout.nb_channels,
                                                    tgt->freq, tgt->fmt, 1);
    if (tgt->frame_size <= 0 || tgt->bytes_per_sec <= 0) {
        return AVERROR(EINVAL);
    }

    return 0;
}

static int write_packets(AVCodecContext *avctx, AVStream *st) {
    int ret;

    for (;;) {
        ret = avcodec_receive_packet(avctx, enc.pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            return ret;
        }
        av_packet_rescale_ts(enc.pkt, avctx->time_base, st->time_base);
        enc.pkt->stream_index = st->index;
        ret = av_interleaved_write_frame(enc.oc, enc.pkt);
        if (ret < 0) {
            return ret;
        }
    }
}

static int64_t video_ticks(double secs) {
    return llrint(secs / av_q2d(ENCODE_VIDEO_TB));
}

static double video_secs(int64_t ticks) {
    return ticks * av_q2d(ENCODE_VIDEO_TB);
}

static double video_duration(const Frame *vp) {
    return vp->duration > 0.0 && vp->duration < ENCODE_SYNC_MAX
        ? vp->duration
        : ENCODE_FALLBACK_FRAME_DURATION;
}

static const char *pix_fmt_name(int format) {
    const char *name = av_get_pix_fmt_name(format);

    return name ? name : "?";
}

static int rescale_video(const AVFrame *src, AVFrame **dst) {
    AVCodecContext *avctx = enc.venc;
    AVFrame *out;
    int ret;

    if (!enc.v_rescale_warned) {
        enc.v_rescale_warned = 1;
        log_warn("The video turns into %dx%d %s partway, so scaling it to %dx%d.\n",
                 src->width, src->height, pix_fmt_name(src->format),
                 avctx->width, avctx->height);
    }
    enc.sws = sws_getCachedContext(enc.sws, src->width, src->height, src->format,
                                   avctx->width, avctx->height, avctx->pix_fmt,
                                   SWS_BICUBIC, NULL, NULL, NULL);
    if (!enc.sws) {
        return AVERROR(EINVAL);
    }
    out = av_frame_alloc();
    if (!out) {
        return AVERROR(ENOMEM);
    }
    out->format = avctx->pix_fmt;
    out->width = avctx->width;
    out->height = avctx->height;
    if ((ret = av_frame_get_buffer(out, 0)) < 0 ||
        (ret = av_frame_copy_props(out, src)) < 0 ||
        (ret = sws_scale(enc.sws, (const uint8_t *const *)src->data, src->linesize,
                         0, src->height, out->data, out->linesize)) < 0) {
        av_frame_free(&out);
        return ret;
    }
    *dst = out;

    return 0;
}

static int send_video(AVFrame *frame, int64_t pts, int64_t duration) {
    AVCodecContext *avctx = enc.venc;
    AVFrame *scaled = NULL;
    int ret;

    if (frame->format != avctx->pix_fmt ||
        frame->width < avctx->width || frame->width > avctx->width + 1 ||
        frame->height < avctx->height || frame->height > avctx->height + 1) {
        if ((ret = rescale_video(frame, &scaled)) < 0) {
            return ret;
        }
        frame = scaled;
    }

    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->pts = pts;
    frame->duration = FFMAX(duration, 1);
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    frame->flags &= ~AV_FRAME_FLAG_KEY;
    ret = avcodec_send_frame(avctx, frame);
    av_frame_free(&scaled);
    if (ret < 0) {
        return ret;
    }

    return write_packets(avctx, enc.vst);
}

static int open_video_encoder(const Frame *vp) {
    const AVFrame *frame = vp->frame;
    AVDictionary *opts = NULL;
    AVCodecContext *avctx;
    AVRational sar = frame->sample_aspect_ratio;
    int ret;

    avctx = avcodec_alloc_context3(enc.vcodec);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }
    avctx->width = FFMAX(2, frame->width & ~1);
    avctx->height = FFMAX(2, frame->height & ~1);
    avctx->pix_fmt = enc.pix_fmts[0];
    avctx->time_base = ENCODE_VIDEO_TB;
    avctx->framerate = av_d2q(1.0 / video_duration(vp), 1001000);
    if (aspect_override_active()) {
        sar = aspect_override_sar(frame->width, frame->height, sar);
    }
    if (sar.num > 0 && sar.den > 0) {
        avctx->sample_aspect_ratio = sar;
    }
    avctx->color_range = frame->color_range;
    avctx->color_primaries = frame->color_primaries;
    avctx->color_trc = frame->color_trc;
    avctx->colorspace = frame->colorspace;
    avctx->chroma_sample_location = frame->chroma_location;
    avctx->flags |= AV_CODEC_FLAG_FRAME_DURATION;
    if (enc.oformat->flags & AVFMT_GLOBALHEADER) {
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (!strcmp(enc.oformat->name, "mp4")) {
        avctx->max_b_frames = 0;
    }
    if (!strcmp(enc.vcodec->name, "libx264")) {
        av_dict_set(&opts, "crf", ENCODE_X264_CRF, 0);
    } else {
        avctx->flags |= AV_CODEC_FLAG_QSCALE;
        avctx->global_quality = FF_QP2LAMBDA * ENCODE_MPEG4_QSCALE;
    }
    ret = avcodec_open2(avctx, enc.vcodec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        avcodec_free_context(&avctx);
        return ret;
    }
    enc.venc = avctx;

    return 0;
}

static int setup_audio(void) {
    AVCodecContext *avctx = enc.aenc;
    int ret;

    enc.fifo = av_audio_fifo_alloc(avctx->sample_fmt, avctx->ch_layout.nb_channels,
                                   ENCODE_SILENCE_CHUNK);
    enc.silence = av_frame_alloc();
    if (!enc.fifo || !enc.silence) {
        return AVERROR(ENOMEM);
    }
    enc.silence->format = avctx->sample_fmt;
    enc.silence->sample_rate = avctx->sample_rate;
    enc.silence->nb_samples = ENCODE_SILENCE_CHUNK;
    if ((ret = av_channel_layout_copy(&enc.silence->ch_layout, &avctx->ch_layout)) < 0 ||
        (ret = av_frame_get_buffer(enc.silence, 0)) < 0) {
        return ret;
    }

    return av_samples_set_silence(enc.silence->extended_data, 0, ENCODE_SILENCE_CHUNK,
                                  avctx->ch_layout.nb_channels, avctx->sample_fmt);
}

static int open_output(void) {
    AVFormatContext *oc = NULL;
    int ret;

    ret = avformat_alloc_output_context2(&oc, enc.oformat, NULL, output_filename);
    if (ret < 0) {
        return ret;
    }
    enc.oc = oc;
    oc->max_interleave_delta = ENCODE_INTERLEAVE_MAX * AV_TIME_BASE;
    if ((ret = av_dict_copy(&oc->metadata, enc.metadata, 0)) < 0) {
        return ret;
    }
    if (enc.venc) {
        enc.vst = avformat_new_stream(oc, NULL);
        if (!enc.vst) {
            return AVERROR(ENOMEM);
        }
        if ((ret = avcodec_parameters_from_context(enc.vst->codecpar, enc.venc)) < 0) {
            return ret;
        }
        enc.vst->time_base = enc.venc->time_base;
        enc.vst->avg_frame_rate = enc.venc->framerate;
        enc.vst->sample_aspect_ratio = enc.venc->sample_aspect_ratio;
    }
    if (enc.aenc) {
        enc.ast = avformat_new_stream(oc, NULL);
        if (!enc.ast) {
            return AVERROR(ENOMEM);
        }
        if ((ret = avcodec_parameters_from_context(enc.ast->codecpar, enc.aenc)) < 0) {
            return ret;
        }
        enc.ast->time_base = enc.aenc->time_base;
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE) &&
        (ret = avio_open(&oc->pb, output_filename, AVIO_FLAG_WRITE)) < 0) {
        return ret;
    }

    return avformat_write_header(oc, NULL);
}

static int render_video(AVFrame *src, AVFrame **dst) {
    RenderParams params = {
        .target_rect = {0, 0, src->width, src->height},
        .video_background_type = VIDEO_BACKGROUND_NONE,
    };
    AVFrame *frame;
    int ret;

    if (!enc.renderer) {
        *dst = av_frame_clone(src);
        return *dst ? 0 : AVERROR(ENOMEM);
    }
    frame = av_frame_alloc();
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    frame->format = src->format;
    frame->width = src->width;
    frame->height = src->height;
    if ((ret = av_frame_get_buffer(frame, 0)) < 0 ||
        (ret = av_frame_copy_props(frame, src)) < 0) {
        av_frame_free(&frame);
        return ret;
    }
    /* Otherwise the target would be turned and color managed. */
    av_frame_remove_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    av_frame_remove_side_data(frame, AV_FRAME_DATA_ICC_PROFILE);
    ret = renderer_capture_frame(enc.renderer, src, &params, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }
    *dst = frame;

    return 0;
}

static int open_renderer(const Frame *vp) {
    RendererOpenParams params = {
        .title = program_name,
        .exclude = no_vulkan ? 1u << RENDERER_API_VULKAN : 0,
        .device = gpu_device,
    };
    AVFrame *probe = NULL;
    const char *device;
    char why[512];
    int ret;

    if (supersample_level == SUPERSAMPLE_OFF) {
        return 0;
    }
    if (no_shader_cache && (ret = av_dict_set(&params.opt, "cache", "0", 0)) < 0) {
        return ret;
    }
    ret = renderer_open_offscreen(&params, &enc.renderer, why, sizeof(why));
    av_dict_free(&params.opt);
    if (ret < 0) {
        log_dead("-supersample needs a GPU, but none would render: %s.\n", why);
        return ret;
    }
    if ((ret = renderer_set_supersample(enc.renderer, supersample_level)) < 0 ||
        (ret = render_video(vp->frame, &probe)) < 0) {
        log_dead("The %s renderer cannot supersample: %s.\n",
                 renderer_api_name(enc.renderer), av_err2str(ret));
        return ret;
    }
    av_frame_free(&probe);

    device = renderer_device_name(enc.renderer);
    if (device) {
        log_info("Supersampling on %s (%s).\n", renderer_api_name(enc.renderer),
                 device);
    } else {
        log_info("Supersampling on %s.\n", renderer_api_name(enc.renderer));
    }

    return 0;
}

static int start(VideoState *is, const Frame *vp, const Frame *af, int audio_coming) {
    double v0 = vp ? vp->pts : LACHESIS_NAN;
    double a0 = af ? af->pts : LACHESIS_NAN;
    int ret;

    enc.hold_max = FFMAX(is->max_frame_duration, ENCODE_SYNC_MAX);
    enc.has_video = vp != NULL;
    enc.has_audio = af != NULL || audio_coming;
    if (enc.want_video && !enc.has_video) {
        log_warn("The video gave no pictures, so the output has no video.\n");
    }
    if (enc.want_audio && !enc.has_audio) {
        log_warn("The audio gave no sound, so the output has no audio.\n");
        avcodec_free_context(&enc.aenc);
    }
    if (!enc.has_video && !enc.has_audio) {
        log_dead("Nothing in '%s' could be decoded.\n", is->filename);
        return AVERROR_INVALIDDATA;
    }

    if (!isnan(v0) && !isnan(a0) && fabs(v0 - a0) >= enc.hold_max) {
        log_verbose("The video starts %+.3f s from the audio, so lining their starts up.\n",
                    v0 - a0);
        enc.v_shift = a0 - v0;
        v0 = a0;
    }
    if (!isnan(v0) && !isnan(a0)) {
        enc.origin = FFMIN(v0, a0);
    } else if (!isnan(a0)) {
        enc.origin = a0;
    } else if (!isnan(v0)) {
        enc.origin = v0;
    }

    if (enc.has_audio) {
        enc.anchor_src = isnan(a0) ? enc.origin : a0;
        enc.anchor_out = enc.anchor_src - enc.origin;
        enc.anchor_valid = 1;
        if ((ret = setup_audio()) < 0) {
            return ret;
        }
    }
    if (vp && (ret = open_renderer(vp)) < 0) {
        return ret;
    }
    if (vp && (ret = open_video_encoder(vp)) < 0) {
        log_dead("Could not open the %s encoder: %s.\n", enc.vcodec->name,
                 av_err2str(ret));
        return ret;
    }
    if ((ret = open_output()) < 0) {
        log_dead("Could not start writing '%s': %s.\n", output_filename,
                 av_err2str(ret));
        return ret;
    }
    enc.started = 1;

    if (enc.venc && enc.aenc) {
        log_info("Converting to '%s' with %s and %s.\n", output_filename,
                 enc.vcodec->name, enc.acodec->name);
    } else {
        log_info("Converting to '%s' with %s.\n", output_filename,
                 enc.venc ? enc.vcodec->name : enc.acodec->name);
    }

    return 0;
}

static int drain_audio(int final) {
    AVCodecContext *avctx = enc.aenc;
    int frame_size = avctx->frame_size > 0 ? avctx->frame_size : 1024;
    int pad = avctx->frame_size > 0 &&
        !(avctx->codec->capabilities &
          (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE));
    int ret;

    for (;;) {
        int avail = av_audio_fifo_size(enc.fifo);
        int n = FFMIN(avail, frame_size);
        AVFrame *frame;

        if (avail < frame_size && !(final && avail > 0)) {
            return 0;
        }
        frame = av_frame_alloc();
        if (!frame) {
            return AVERROR(ENOMEM);
        }
        frame->format = avctx->sample_fmt;
        frame->sample_rate = avctx->sample_rate;
        frame->nb_samples = pad ? frame_size : n;
        if ((ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout)) < 0 ||
            (ret = av_frame_get_buffer(frame, 0)) < 0) {
            av_frame_free(&frame);
            return ret;
        }
        if (av_audio_fifo_read(enc.fifo, (void **)frame->extended_data, n) < n) {
            av_frame_free(&frame);
            return AVERROR_BUG;
        }
        if (frame->nb_samples > n) {
            av_samples_set_silence(frame->extended_data, n, frame->nb_samples - n,
                                   avctx->ch_layout.nb_channels, avctx->sample_fmt);
        }
        frame->pts = enc.a_sent;
        enc.a_sent += frame->nb_samples;
        ret = avcodec_send_frame(avctx, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            return ret;
        }
        if ((ret = write_packets(avctx, enc.ast)) < 0) {
            return ret;
        }
    }
}

static int push_samples(uint8_t *const *data, int nb_samples) {
    int ret = av_audio_fifo_write(enc.fifo, (void *const *)data, nb_samples);

    if (ret < 0) {
        return ret;
    }
    if (ret < nb_samples) {
        return AVERROR(ENOMEM);
    }
    enc.a_samples += nb_samples;

    return drain_audio(0);
}

static int push_silence(int64_t nb_samples) {
    while (nb_samples > 0) {
        int n = (int)FFMIN(nb_samples, ENCODE_SILENCE_CHUNK);
        int ret = push_samples(enc.silence->extended_data, n);

        if (ret < 0) {
            return ret;
        }
        nb_samples -= n;
    }

    return 0;
}

static double audio_out(void) {
    return enc.a_samples / (double)enc.aenc->sample_rate;
}

static double audio_next_out(void) {
    return enc.a_started ? audio_out() : enc.anchor_out;
}

static int take_audio(const Frame *af) {
    AVFrame *frame = af->frame;
    AVCodecContext *avctx = enc.aenc;
    double rate = avctx->sample_rate;
    double src = af->pts;
    char at[16];
    int ret;

    if (frame->format != avctx->sample_fmt || frame->sample_rate != avctx->sample_rate ||
        frame->ch_layout.nb_channels != avctx->ch_layout.nb_channels) {
        if (!enc.a_mismatch_warned) {
            enc.a_mismatch_warned = 1;
            log_warn("Skipping audio that came out of the filters as %s at %d Hz.\n",
                     av_get_sample_fmt_name(frame->format), frame->sample_rate);
        }
        return 0;
    }
    if (isnan(src)) {
        src = enc.a_started ? enc.a_next_src : enc.anchor_src;
    }
    if (!enc.a_started) {
        int64_t lead = llrint((src - enc.origin) * rate);

        enc.a_started = 1;
        if (lead > 0 && (ret = push_silence(lead)) < 0) {
            return ret;
        }
    } else if (fabs(src - enc.a_next_src) > ENCODE_AUDIO_JUMP) {
        enc.a_jumps++;
        format_time(at, sizeof(at), audio_out());
        log_verbose("The audio timestamps jump by %+.3f s at %s.\n",
                    src - enc.a_next_src, at);
    }
    enc.anchor_src = src;
    enc.anchor_out = audio_out();
    enc.anchor_valid = 1;
    enc.a_next_src = src + frame->nb_samples / rate;

    return push_samples(frame->extended_data, frame->nb_samples);
}

static double place_video(const Frame *vp, double *src_out, int *synced) {
    double src = isnan(vp->pts) ? LACHESIS_NAN : vp->pts + enc.v_shift;
    double expected = LACHESIS_NAN;
    double mapped = LACHESIS_NAN;

    if (isnan(src) && enc.v_frames && !isnan(enc.v_last_src)) {
        src = enc.v_last_src + enc.v_last_dur;
    }
    if (enc.v_frames) {
        double step = src - enc.v_last_src;

        if (!(step > 0.0 && step < ENCODE_SYNC_MAX)) {
            step = enc.v_last_dur;
        }
        expected = enc.v_last_out + step;
    }
    if (!isnan(src)) {
        if (enc.anchor_valid) {
            mapped = enc.anchor_out + (src - enc.anchor_src);
        } else if (!enc.v_frames || !enc.has_audio) {
            /* Without sound playback goes by a clock that starts with the
             * first picture. */
            mapped = src - enc.origin;
        }
    }
    *src_out = src;
    *synced = !isnan(mapped) &&
        (isnan(expected) || fabs(mapped - expected) < ENCODE_SYNC_MAX ||
         (mapped > expected && mapped - expected < enc.hold_max));

    if (*synced) {
        double paced = enc.v_last_out + enc.v_last_dur;

        return enc.v_frames && fabs(mapped - paced) < ENCODE_PACE_SNAP ? paced : mapped;
    }

    return isnan(expected) ? 0.0 : expected;
}

static int take_video(const Frame *vp, double out, double src, int synced) {
    AVFrame *frame = NULL;
    int64_t ts = FFMAX(video_ticks(out), 0);
    char at[16];
    int ret = render_video(vp->frame, &frame);

    if (ret < 0) {
        return ret;
    }
    if (enc.has_audio && enc.v_frames && synced == enc.v_free_running) {
        enc.v_free_running = !synced;
        format_time(at, sizeof(at), out);
        if (enc.v_free_running) {
            log_verbose("The video timestamps leave the audio at %s, so the "
                        "pictures follow each other until they line up again.\n",
                        at);
        } else {
            log_verbose("The video timestamps line up with the audio again at %s.\n",
                        at);
        }
    }

    if (enc.pending && ts <= enc.pending_ts) {
        av_frame_free(&enc.pending);
        enc.v_dropped += !enc.pending_resent;
        ts = enc.pending_ts;
    } else if (enc.pending) {
        ret = send_video(enc.pending, enc.pending_ts, ts - enc.pending_ts);
        av_frame_free(&enc.pending);
    }
    enc.pending = frame;
    enc.pending_ts = ts;
    enc.pending_resent = 0;
    enc.v_last_src = src;
    enc.v_last_out = video_secs(ts);
    enc.v_last_dur = video_duration(vp);
    enc.v_frames++;

    return ret;
}

static int resend_held(void) {
    int64_t now = video_ticks(audio_out());
    AVFrame *copy;
    int ret;

    if (!enc.pending || now - enc.pending_ts < video_ticks(ENCODE_HOLD_RESEND)) {
        return 0;
    }
    if (!(copy = av_frame_clone(enc.pending))) {
        return AVERROR(ENOMEM);
    }
    ret = send_video(enc.pending, enc.pending_ts, now - enc.pending_ts);
    av_frame_free(&enc.pending);
    enc.pending = copy;
    enc.pending_ts = now;
    enc.pending_resent = 1;

    return ret;
}

static int stream_done(const Decoder *d, const PacketQueue *q, FrameQueue *f) {
    return d->finished == q->serial && frame_queue_nb_remaining(f) == 0;
}

static void drop_stale(FrameQueue *f, const PacketQueue *q) {
    while (frame_queue_nb_remaining(f) > 0 && frame_queue_peek(f)->serial != q->serial) {
        frame_queue_next(f);
    }
}

static void discard_frames(FrameQueue *f) {
    while (frame_queue_nb_remaining(f) > 0) {
        frame_queue_next(f);
    }
}

static int starved(const VideoState *is, const PacketQueue *q) {
    return q->nb_packets == 0 && demux_queues_full(is);
}

static int stray_ahead(VideoState *is, double out, double src) {
    const Frame *next;

    if (!enc.v_frames || isnan(src) ||
        out - (enc.v_last_out + enc.v_last_dur) <= ENCODE_STRAY_AHEAD) {
        return 0;
    }
    if (frame_queue_nb_remaining(&is->pictq) < 2) {
        return is->viddec.finished != is->videoq.serial &&
                !starved(is, &is->videoq)
            ? -1
            : 0;
    }
    next = frame_queue_peek_next(&is->pictq);
    if (next->serial != is->videoq.serial || isnan(next->pts)) {
        return 0;
    }

    return next->pts + enc.v_shift < src - ENCODE_STRAY_AHEAD / 2;
}

static void wait_for_frames(FrameQueue *f) {
    if (!f) {
        SDL_Delay(1);
        return;
    }
    SDL_LockMutex(f->mutex);
    if (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        SDL_WaitConditionTimeout(f->cond, f->mutex, ENCODE_WAIT_MS);
    }
    SDL_UnlockMutex(f->mutex);
}

static int encode_step(VideoState *is, FrameQueue **wait_on) {
    Frame *vp = NULL;
    Frame *af = NULL;
    int v_done = 1;
    int a_done = 1;
    double v_out = 0.0;
    double v_src = LACHESIS_NAN;
    int synced = 0;
    int ret;

    *wait_on = NULL;
    if (!SDL_GetAtomicInt(&is->streams_selected) || is->seek_req) {
        return 0;
    }
    if (!enc.started) {
        enc.want_video = is->video_st && enc.vcodec;
        enc.want_audio = is->audio_st && enc.aenc;
        if (!enc.want_video && !enc.want_audio) {
            log_dead("'%s' has nothing to convert.\n", is->filename);
            return AVERROR_INVALIDDATA;
        }
    }
    if (enc.want_video) {
        drop_stale(&is->pictq, &is->videoq);
        v_done = stream_done(&is->viddec, &is->videoq, &is->pictq);
        if (!v_done && frame_queue_nb_remaining(&is->pictq) > 0) {
            vp = frame_queue_peek(&is->pictq);
        }
    }
    if (enc.want_audio) {
        drop_stale(&is->sampq, &is->audioq);
        a_done = stream_done(&is->auddec, &is->audioq, &is->sampq);
        if (!a_done && frame_queue_nb_remaining(&is->sampq) > 0) {
            af = frame_queue_peek(&is->sampq);
        }
    }

    if (!enc.started) {
        if (enc.want_video && !vp && !v_done && !starved(is, &is->videoq)) {
            *wait_on = &is->pictq;
            return 0;
        }
        if (enc.want_audio && !af && !a_done && !starved(is, &is->audioq)) {
            *wait_on = &is->sampq;
            return 0;
        }
        if ((ret = start(is, vp, af, enc.want_audio && !a_done)) < 0) {
            return ret;
        }
    }
    if (enc.want_video && !enc.has_video) {
        discard_frames(&is->pictq);
        vp = NULL;
        v_done = 1;
    }
    if (enc.want_audio && !enc.has_audio) {
        discard_frames(&is->sampq);
        af = NULL;
        a_done = 1;
    }
    if (v_done && a_done) {
        return AVERROR_EOF;
    }

    if (vp) {
        v_out = place_video(vp, &v_src, &synced);
        ret = stray_ahead(is, v_out, v_src);
        if (ret < 0) {
            *wait_on = NULL;
            return 0;
        }
        if (ret) {
            enc.v_strays++;
            log_verbose("Leaving out a picture stamped %.3f s ahead of the ones around it.\n",
                        v_out - enc.v_last_out);
            frame_queue_next(&is->pictq);
            return 1;
        }
    }
    if (vp && (af ? v_out < audio_next_out() : a_done || v_out < audio_next_out() || starved(is, &is->audioq))) {
        ret = take_video(vp, v_out, v_src, synced);
        frame_queue_next(&is->pictq);
    } else if (af && (vp || v_done || (enc.v_frames && audio_next_out() < enc.v_last_out) || starved(is, &is->videoq))) {
        ret = take_audio(af);
        frame_queue_next(&is->sampq);
        if (ret >= 0) {
            ret = resend_held();
        }
    } else {
        *wait_on = vp || v_done ? &is->sampq : &is->pictq;
        return 0;
    }

    return ret < 0 ? ret : 1;
}

static int input_failed(void) {
    SDL_Event event;

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, FF_QUIT_EVENT, FF_QUIT_EVENT) > 0) {
        if (event.user.code == FF_QUIT_REASON_ERROR) {
            return 1;
        }
    }

    return 0;
}

static double output_time(void) {
    double t = enc.v_frames ? enc.v_last_out + enc.v_last_dur : 0.0;

    if (enc.has_audio && enc.aenc) {
        t = FFMAX(t, audio_out());
    }

    return t;
}

static double expected_length(VideoState *is) {
    double length = playhead_length(is);

    if (start_time != AV_NOPTS_VALUE) {
        length -= start_time / (double)AV_TIME_BASE;
    }
    if (play_duration != AV_NOPTS_VALUE) {
        double limit = play_duration / (double)AV_TIME_BASE;

        if (length <= 0.0 || limit < length) {
            length = limit;
        }
    }

    return length;
}

static void report_progress(VideoState *is) {
    int64_t now = av_gettime_relative();
    double done, length, wall;
    char pos[16], len[16], line[96];

    if (!enc.started || !log_status_available() ||
        now - enc.progress_us < ENCODE_PROGRESS_US) {
        return;
    }
    enc.progress_us = now;
    done = output_time();
    length = expected_length(is);
    wall = (now - enc.start_us) / 1000000.0;
    format_time(pos, sizeof(pos), done);
    if (length > 0.0) {
        format_time(len, sizeof(len), length);
        snprintf(line, sizeof(line), "Converting %s / %s (%d%%) at %.1fx", pos, len,
                 (int)FFMIN(100.0, 100.0 * done / length),
                 wall > 0.0 ? done / wall : 0.0);
    } else {
        snprintf(line, sizeof(line), "Converting %s at %.1fx", pos,
                 wall > 0.0 ? done / wall : 0.0);
    }
    log_status_set(line);
}

static int keep_error(int ret, int err) {
    return ret < 0 ? ret : err;
}

static int finish(VideoState *is, int ret, int interrupted) {
    char took[16], length[16];
    int err;

    log_status_set("");
    if (!enc.started) {
        return ret < 0 ? ret : AVERROR_EXIT;
    }

    if (enc.has_audio && !enc.a_started) {
        if (!interrupted) {
            log_warn("The audio never started, so it is silent.\n");
        }
        enc.a_started = 1;
        ret = keep_error(ret, push_silence(llrint(output_time() * enc.aenc->sample_rate)));
    }

    if (enc.pending) {
        int64_t duration = FFMAX(video_ticks(enc.v_last_dur), 1);
        int64_t end = enc.pending_ts + duration;
        AVFrame *last = NULL;

        if (enc.has_audio) {
            end = FFMAX(end, av_rescale_q(enc.a_samples, enc.aenc->time_base, ENCODE_VIDEO_TB));
        }
        if (end - enc.pending_ts > duration && !(last = av_frame_clone(enc.pending))) {
            ret = keep_error(ret, AVERROR(ENOMEM));
        }
        err = send_video(enc.pending, enc.pending_ts,
                         (last ? end - duration : end) - enc.pending_ts);
        if (last && err >= 0) {
            err = send_video(last, end - duration, duration);
        }
        av_frame_free(&last);
        av_frame_free(&enc.pending);
        ret = keep_error(ret, err);
    }
    if (enc.venc) {
        err = avcodec_send_frame(enc.venc, NULL);
        ret = keep_error(ret, err < 0 ? err : write_packets(enc.venc, enc.vst));
    }
    if (enc.aenc) {
        err = drain_audio(1);
        if (err >= 0) {
            err = avcodec_send_frame(enc.aenc, NULL);
        }
        ret = keep_error(ret, err < 0 ? err : write_packets(enc.aenc, enc.ast));
    }
    ret = keep_error(ret, av_write_trailer(enc.oc));
    ret = keep_error(ret, avio_closep(&enc.oc->pb));

    format_time(length, sizeof(length), output_time());
    format_time(took, sizeof(took), (av_gettime_relative() - enc.start_us) / 1000000.0);
    if (ret < 0) {
        log_dead("Converting '%s' failed after %s: %s.\n", is->filename, length,
                 av_err2str(ret));
        return ret;
    }
    if (enc.a_jumps) {
        log_info("The audio timestamps jumped %d time%s, so the sound runs on "
                 "without gaps the way playback has it.\n",
                 enc.a_jumps, enc.a_jumps == 1 ? "" : "s");
    }
    if (enc.v_dropped) {
        log_info("Left out %" PRId64 " picture%s that came too late, as playback "
                 "would.\n",
                 enc.v_dropped, enc.v_dropped == 1 ? "" : "s");
    }
    if (enc.v_strays) {
        log_info("Left out %" PRId64 " picture%s with a timestamp far ahead of "
                 "the rest.\n",
                 enc.v_strays, enc.v_strays == 1 ? "" : "s");
    }
    log_info("%s %s to '%s' in %s.\n", interrupted ? "Stopped early after writing" : "Wrote",
             length, output_filename, took);

    return 0;
}

static void free_encoder(void) {
    av_frame_free(&enc.pending);
    av_frame_free(&enc.silence);
    av_audio_fifo_free(enc.fifo);
    enc.fifo = NULL;
    sws_freeContext(enc.sws);
    enc.sws = NULL;
    avcodec_free_context(&enc.venc);
    avcodec_free_context(&enc.aenc);
    if (enc.renderer && renderer_destroy(enc.renderer)) {
        av_freep(&enc.renderer);
    }
    if (enc.oc) {
        avio_closep(&enc.oc->pb);
        avformat_free_context(enc.oc);
        enc.oc = NULL;
    }
    av_packet_free(&enc.pkt);
    av_dict_free(&enc.metadata);
}

av_noreturn void encoder_run(VideoState *is) {
    FrameQueue *wait_on;
    int interrupted = 0;
    int ret = 0;

    enc.start_us = av_gettime_relative();
    for (;;) {
        if (poll_quit_request()) {
            interrupted = 1;
            break;
        }
        if (input_failed()) {
            ret = AVERROR(EIO);
            break;
        }
        ret = encode_step(is, &wait_on);
        if (ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            break;
        }
        if (!ret) {
            wait_for_frames(wait_on);
        }
        report_progress(is);
    }

    ret = finish(is, ret, interrupted);
    free_encoder();
    if (ret < 0 || interrupted) {
        exit_status = 1;
    }
    do_exit(is);
}
