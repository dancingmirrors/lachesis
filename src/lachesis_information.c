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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libavutil/time.h>

#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_osd.h"

static const char *active_hwaccel = NULL;

static char audio_device_driver_line[96] = "";
static char audio_device_format_line[96] = "";
static char media_info_vout_line[128] = "";

static const char *media_info_renderer(void) {
    return vk_renderer ? "vulkan (libplacebo)" : "SDL (software)";
}

static const char *media_info_hwaccel(void) {
    return active_hwaccel ? active_hwaccel : "none (software decoding)";
}

static void media_info_video_line(const VideoState *is, char *buf, size_t sz) {
    AVCodecParameters *par = is->video_st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(is->ic, is->video_st, NULL);
    AVRational fr = av_guess_frame_rate(is->ic, (AVStream *)is->video_st, NULL);
    AVRational dar = {0, 1};
    char dar_buf[32];

    if (sar.num && sar.den) {
        av_reduce(&dar.num, &dar.den,
                  par->width * (int64_t)sar.num,
                  par->height * (int64_t)sar.den, 1024 * 1024);
        snprintf(dar_buf, sizeof(dar_buf), "%d:%d", dar.num, dar.den);
    } else {
        snprintf(dar_buf, sizeof(dar_buf), "unavailable");
    }

    char fps_buf[32];
    fps_buf[0] = '\0';
    if (fr.num && fr.den) {
        snprintf(fps_buf, sizeof(fps_buf), ", %.4g fps", av_q2d(fr));
    }
    snprintf(buf, sz, "%s, %dx%d, SAR %d:%d DAR %s%s",
             avcodec_get_name(par->codec_id), par->width, par->height,
             sar.num ? sar.num : 0, sar.den ? sar.den : 1, dar_buf, fps_buf);
}

static void media_info_audio_line(const VideoState *is, char *buf, size_t sz) {
    AVCodecParameters *par = is->audio_st->codecpar;
    char chl[64];

    av_channel_layout_describe(&par->ch_layout, chl, sizeof(chl));
    snprintf(buf, sz, "%s, %d Hz, %s",
             avcodec_get_name(par->codec_id), par->sample_rate, chl);
}

static int media_info_duration_line(const VideoState *is, char *buf, size_t sz) {
    if (is->ic && is->ic->duration != AV_NOPTS_VALUE) {
        format_time(buf, sz, (double)is->ic->duration / AV_TIME_BASE);
        return 1;
    }
    if (sz) {
        buf[0] = '\0';
    }
    return 0;
}

void print_current_file(const VideoState *is) {
    if (is) {
        const char *url = is->ytdl_source_url ? is->ytdl_source_url : is->filename;
        if (url) {
            log_info("%s\n", url);
        }
    }
}

void print_stream_info(const VideoState *is) {
    if (!is) {
        return;
    }

    char line[256];

    log_info("Using renderer: %s\n", media_info_renderer());
    log_info("Using hwaccel: %s\n", media_info_hwaccel());

    if (is->video_st) {
        media_info_video_line(is, line, sizeof(line));
        log_info("Video: %s\n", line);
    }

    if (is->audio_st) {
        media_info_audio_line(is, line, sizeof(line));
        log_info("Audio: %s\n", line);
    }

    if (media_info_duration_line(is, line, sizeof(line))) {
        log_info("Duration: %s\n", line);
    }
}

void format_media_info(const VideoState *is, char *buf, size_t bufsz) {
    if (bufsz == 0) {
        return;
    }
    buf[0] = '\0';
    if (!is) {
        return;
    }

    size_t len = 0;
    char tmp[256];
    char sub[192];

    /* clang-format off */
#define MI_LINE(...)                                                       \
    do {                                                                   \
        snprintf(tmp, sizeof(tmp), __VA_ARGS__);                           \
        int _n = snprintf(buf + len, bufsz - len, "%s%s", len ? "\n" : "", \
                          tmp);                                            \
        if (_n > 0) {                                                      \
            len += (size_t)_n;                                             \
            if (len >= bufsz) {                                            \
                len = bufsz - 1;                                           \
            }                                                              \
        }                                                                  \
    } while (0)
    /* clang-format on */

    const char *url = is->ytdl_source_url ? is->ytdl_source_url : is->filename;
    if (url && url[0]) {
        MI_LINE("%s", url);
    }

    if (audio_device_driver_line[0]) {
        MI_LINE("SDL audio device driver: %s", audio_device_driver_line);
    }
    if (audio_device_format_line[0]) {
        MI_LINE("SDL audio device format: %s", audio_device_format_line);
    }

    MI_LINE("Using renderer: %s", media_info_renderer());
    MI_LINE("Using hwaccel: %s", media_info_hwaccel());

    if (is->video_st) {
        media_info_video_line(is, sub, sizeof(sub));
        MI_LINE("Video: %s", sub);
    }
    if (is->audio_st) {
        media_info_audio_line(is, sub, sizeof(sub));
        MI_LINE("Audio: %s", sub);
    }
    if (media_info_duration_line(is, sub, sizeof(sub))) {
        MI_LINE("Duration: %s", sub);
    }
    if (media_info_vout_line[0]) {
        MI_LINE("Video output: %s", media_info_vout_line);
    }

#undef MI_LINE
}

void format_playback_stats(const VideoState *is, char *buf, size_t bufsz) {
    if (bufsz == 0) {
        return;
    }
    buf[0] = '\0';
    if (!is) {
        return;
    }

    static char cached[256];
    static int64_t next_refresh_us = 0;
    int64_t now = av_gettime_relative();
    if (cached[0] && now < next_refresh_us) {
        snprintf(buf, bufsz, "%s", cached);
        return;
    }

    if (!is->video_st) {
        snprintf(cached, sizeof(cached), "No video stream");
    } else {
        int early = is->frame_drops_early;
        int late = is->frame_drops_late;

        char timing[128];
        double acquire = 0.0, render = 0.0, present = 0.0;
        if (vk_renderer &&
            vk_renderer_frame_stats(vk_renderer, &acquire, &render, &present)) {
            snprintf(timing, sizeof(timing),
                     "acquire=%.2fms render=%.2fms present=%.2fms", acquire,
                     render, present);
        } else if (vk_renderer) {
            snprintf(timing, sizeof(timing), "Frame timing: measuring...");
        } else {
            snprintf(timing, sizeof(timing),
                     "Frame timing: unavailable");
        }

        snprintf(cached, sizeof(cached),
                 "Dropped frames: %d (early %d, late %d)\n%s", early + late,
                 early, late, timing);
    }

    next_refresh_us = now + 500000;
    snprintf(buf, bufsz, "%s", cached);
}

/* Make sure we don't somehow inherit the previous file's lines. */
void media_info_reset(void) {
    audio_device_driver_line[0] = '\0';
    audio_device_format_line[0] = '\0';
    media_info_vout_line[0] = '\0';
    osd_invalidate_info();
}

void media_info_set_hwaccel(const char *name) {
    active_hwaccel = name;
}

void media_info_note_audio_driver(const char *driver, int channels, int freq) {
    snprintf(audio_device_driver_line, sizeof(audio_device_driver_line),
             "%s, requested S16 %dch @ %dHz", driver ? driver : "(none)",
             channels, freq);
    log_info("SDL audio device driver: %s\n", audio_device_driver_line);
}

void media_info_note_audio_format(unsigned fmt, int channels, int freq,
                                  int buffer_frames) {
    snprintf(audio_device_format_line, sizeof(audio_device_format_line),
             "fmt=0x%x %dch @ %dHz, buffer=%d frames", fmt, channels, freq,
             buffer_frames);
    log_info("SDL audio device format: %s\n", audio_device_format_line);
}

void media_info_note_video_output(int width, int height, AVRational sar) {
    AVRational dar = {0, 1};
    char dar_buf[32];

    if (sar.num && sar.den) {
        av_reduce(&dar.num, &dar.den, width * (int64_t)sar.num,
                  height * (int64_t)sar.den, 1024 * 1024);
        snprintf(dar_buf, sizeof(dar_buf), "%d:%d", dar.num, dar.den);
    } else {
        snprintf(dar_buf, sizeof(dar_buf), "unavailable");
    }

    snprintf(media_info_vout_line, sizeof(media_info_vout_line),
             "%dx%d, SAR %d:%d DAR %s", width, height,
             sar.num ? sar.num : 0, sar.den ? sar.den : 1, dar_buf);
    log_info("Video output: %s\n", media_info_vout_line);
}
