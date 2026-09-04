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

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libavutil/attributes.h>
#include <libavutil/time.h>

#include "lachesis_deinterlace.h"
#include "lachesis_information.h"
#include "lachesis_internal.h"
#include "lachesis_interpolate.h"
#include "lachesis_log.h"
#include "lachesis_normalize.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_present.h"
#include "lachesis_supersample.h"

static const char *active_hwaccel = NULL;

static char audio_device_driver_line[96] = "";
static char audio_device_format_line[96] = "";
static char audio_passthrough_line[96] = "";
static char media_info_vout_line[128] = "";

static char playback_stats_cached[1024] = "";
static int64_t playback_stats_next_refresh_us = 0;

static const char *media_info_renderer(void) {
    static char buf[320];
    const char *device;

    if (!renderer) {
        return "none";
    }

    device = renderer_device_name(renderer);
    if (device) {
        snprintf(buf, sizeof(buf), "%s via libplacebo (%s)",
                 renderer_api_name(renderer), device);
    } else {
        snprintf(buf, sizeof(buf), "%s via libplacebo",
                 renderer_api_name(renderer));
    }

    return buf;
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
        snprintf(fps_buf, sizeof(fps_buf), ", %.4g FPS", av_q2d(fr));
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
    double length = is->ic ? playhead_length(is) : 0.0;

    if (length > 0.0) {
        format_time(buf, sz, length);
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
    log_info("%s hwaccel: %s\n", active_hwaccel ? "Trying" : "Using",
             media_info_hwaccel());

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

static void av_printf_format(4, 5)
    media_info_append(char *buf, size_t bufsz, size_t *len, const char *fmt,
                      ...) {
    size_t at = *len;
    va_list ap;
    int n;

    if (at + 1 >= bufsz) {
        return;
    }
    if (at) {
        buf[at++] = '\n';
        buf[at] = '\0';
        if (at + 1 >= bufsz) {
            *len = at;
            return;
        }
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + at, bufsz - at, fmt, ap);
    va_end(ap);

    if (n > 0) {
        at += (size_t)n;
        if (at >= bufsz) {
            at = bufsz - 1;
        }
    }
    *len = at;
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
    char sub[256];

#define MI_LINE(...) media_info_append(buf, bufsz, &len, __VA_ARGS__)

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
    if (audio_passthrough_line[0]) {
        MI_LINE("Audio passthrough: %s", audio_passthrough_line);
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

static enum SupersampleState playback_stats_supersample(void) {
    if (!renderer) {
        return SUPERSAMPLE_NO_RENDERER;
    }
    if (benchmark) {
        return SUPERSAMPLE_BENCHMARKING;
    }

    return SUPERSAMPLE_RUNNING;
}

void format_playback_stats(const VideoState *is, char *buf, size_t bufsz) {
    if (bufsz == 0) {
        return;
    }
    buf[0] = '\0';
    if (!is) {
        return;
    }

    char *cached = playback_stats_cached;
    const size_t cached_size = sizeof(playback_stats_cached);
    int64_t now = av_gettime_relative();
    if (cached[0] && now < playback_stats_next_refresh_us) {
        snprintf(buf, bufsz, "%s", cached);
        return;
    }

    if (!is->video_st) {
        if (is->audio_st) {
            snprintf(cached, cached_size, "No video stream\nNormalization: %s",
                     normalize_status());
        } else {
            snprintf(cached, cached_size, "No video stream");
        }
    } else {
        int early = is->frame_drops_early;
        int late = is->frame_drops_late;

        char timing[128];
        double acquire = 0.0, convert = 0.0, render = 0.0, present = 0.0;
        if (renderer &&
            renderer_frame_stats(renderer, &acquire, &convert, &render,
                                 &present)) {
            snprintf(timing, sizeof(timing),
                     "acquire=%.2fms upload=%.2fms render=%.2fms "
                     "present=%.2fms",
                     acquire, convert, render, present);
        } else if (renderer) {
            snprintf(timing, sizeof(timing), "Frame timing: measuring...");
        } else {
            snprintf(timing, sizeof(timing),
                     "Frame timing: unavailable");
        }

        char sync_line[64];
        if (!isnan(is->last_av_diff)) {
            snprintf(sync_line, sizeof(sync_line), "A-V: %+.3fs",
                     is->last_av_diff);
        } else {
            snprintf(sync_line, sizeof(sync_line), "A-V: unavailable");
        }

        char disp_line[160];
        PresentStats ps;
        present_get_stats(&ps);
        const char *hz_from = ps.driver_refresh ? " reported" : "";
        char feedback[32];
        snprintf(feedback, sizeof(feedback), " [%s]",
                 present_source_name(ps.source));
        const char *snap_note = "";
        if (!ps.snapping) {
            snap_note = ", snap off";
        } else if (!ps.locked) {
            snap_note = ", not locked";
        }
        if (ps.measured_hz > 0) {
            snprintf(disp_line, sizeof(disp_line),
                     "Display: %.2f%s Hz (measured %.3f Hz%s, jitter %.1f%%)%s%s",
                     ps.nominal_hz, hz_from, ps.measured_hz,
                     ps.measuring ? ", in use" : "", ps.jitter * 100.0,
                     snap_note, feedback);
        } else if (ps.unsynced) {
            snprintf(disp_line, sizeof(disp_line),
                     "Display: %.2f%s Hz (no usable feedback)%s%s",
                     ps.nominal_hz, hz_from, ps.snapping ? "" : ", snap off",
                     feedback);
        } else if (ps.samples > 0) {
            snprintf(disp_line, sizeof(disp_line),
                     "Display: %.2f%s Hz (measuring, %d/%d)%s%s", ps.nominal_hz,
                     hz_from, ps.samples, ps.samples_needed, snap_note,
                     feedback);
        } else {
            snprintf(disp_line, sizeof(disp_line), "Display: %.2f%s Hz%s%s",
                     ps.nominal_hz, hz_from, snap_note, feedback);
        }

        char audio_line[192];
        if (is->audio_st) {
            snprintf(audio_line, sizeof(audio_line), "\nNormalization: %s",
                     normalize_status());
        } else {
            audio_line[0] = '\0';
        }

        snprintf(cached, cached_size,
                 "Dropped frames: %d (early %d, late %d)\n%s\n%s\n"
                 "Decoding: %s\n"
                 "Interpolation: %s\nDeinterlacing: %s\nSupersampling: %s\n%s%s",
                 early + late, early, late, sync_line, disp_line,
                 degrade_status(is), interpolate_status(), deinterlace_status(is),
                 supersample_status(supersample_level,
                                    playback_stats_supersample()),
                 timing, audio_line);
    }

    playback_stats_next_refresh_us = now + 500000;
    snprintf(buf, bufsz, "%s", cached);
}

/* Make sure we don't somehow inherit the previous file's lines. */
void media_info_reset(void) {
    audio_device_driver_line[0] = '\0';
    audio_device_format_line[0] = '\0';
    audio_passthrough_line[0] = '\0';
    media_info_vout_line[0] = '\0';
    active_hwaccel = NULL;
    playback_stats_cached[0] = '\0';
    playback_stats_next_refresh_us = 0;
    osd_invalidate_info();
}

void media_info_set_hwaccel(const char *name) {
    active_hwaccel = name;
}

void media_info_note_hw_frame(int is_hw) {
    if (is_hw || !active_hwaccel) {
        return;
    }
    log_info("Using software decoding\n");
    active_hwaccel = NULL;
    osd_invalidate_info();
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

void media_info_clear_audio_passthrough(void) {
    audio_passthrough_line[0] = '\0';
}

void media_info_note_audio_passthrough(const char *codec, int hd, int channels,
                                       int freq) {
    snprintf(audio_passthrough_line, sizeof(audio_passthrough_line),
             "IEC 61937 (%s%s), %dch @ %d Hz", codec ? codec : "(unknown)",
             hd ? " HD" : "", channels, freq);
    log_info("Audio passthrough: %s\n", audio_passthrough_line);
}

void media_info_note_video_output(int width, int height, AVRational sar,
                                  AVRational frame_rate) {
    AVRational dar = {0, 1};
    char dar_buf[32];
    char fps_buf[32];

    if (sar.num && sar.den) {
        av_reduce(&dar.num, &dar.den, width * (int64_t)sar.num,
                  height * (int64_t)sar.den, 1024 * 1024);
        snprintf(dar_buf, sizeof(dar_buf), "%d:%d", dar.num, dar.den);
    } else {
        snprintf(dar_buf, sizeof(dar_buf), "unavailable");
    }

    fps_buf[0] = '\0';
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        snprintf(fps_buf, sizeof(fps_buf), ", %.4g FPS", av_q2d(frame_rate));
    }

    snprintf(media_info_vout_line, sizeof(media_info_vout_line),
             "%dx%d, SAR %d:%d DAR %s%s", width, height,
             sar.num ? sar.num : 0, sar.den ? sar.den : 1, dar_buf, fps_buf);
    log_info("Video output: %s\n", media_info_vout_line);
}
