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
#include "version.h"

#if LACHESIS_HAVE_LIBPLACEBO
#include <libplacebo/config.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "lachesis_cmdutils.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"

const AVInputFormat *file_iformat;
const char *window_title;
int cmd_width = 0;
int cmd_height = 0;
int screen_left = SDL_WINDOWPOS_CENTERED;
int screen_top = SDL_WINDOWPOS_CENTERED;
int audio_disable;
int video_disable;
int subtitle_disable;
const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
int seek_by_bytes = -1;
float seek_interval = 5.0;
int display_disable;
int benchmark;
int borderless;
int alwaysontop;
int startup_volume = 100;
int av_sync_type = AV_SYNC_AUDIO_MASTER;
int av_sync_type_explicit = 0;
int skip_to_keyframe = 0;
int64_t start_time = AV_NOPTS_VALUE;
int64_t play_duration = AV_NOPTS_VALUE;
int fast = 0;
int genpts = 0;
int decoder_reorder_pts = -1;
int keep_open;
int shuffle;
int reverse_playlist;
int start_paused;
int exit_on_keydown;
int exit_on_mousedown;
int loop = 1;
int framedrop = -1;
int infinite_buffer = -1;
float opt_cache_secs = -1.0f;
int opt_cache_size_mb = -1;
const char *audio_codec_name;
const char *subtitle_codec_name;
const char *video_codec_name;
const char **vfilters_list = NULL;
int nb_vfilters = 0;
char *afilters_opt = NULL;
int autorotate = 1;
int video_rotate = 0;
int find_stream_info = 1;
int filter_nbthreads = 0;
int enable_vulkan = 1;
int disable_vulkan = 0;
char *vulkan_params = NULL;
char *vulkan_swap_mode = NULL;
int no_shader_cache = 0;
char *shader_cache_dir = NULL;
const char *icc_profile = NULL;
char *video_background = NULL;
const char *hwaccel = NULL;
int no_hwaccel = 0;
int video_unscaled = 0;
int enable_360sbs = 0;
int enable_360tb = 0;
int is_fullscreen = 1;
int start_windowed = 0;
float autofit_larger = 0.85f;
int global_muted = 0;
int ytdl_disable = 0;
const char *ytdl_path = NULL;
const char *ytdl_format = NULL;
int allow_delete = 0;
int terminal_quit_disable = 0;
/* XXX: Is 260% loud enough to void your warranty? */
int allow_volume_boost = 1;

static int opt_add_vfilter(void *optctx av_unused, const char *opt av_unused,
                           const char *arg) {
    int ret = GROW_ARRAY(vfilters_list, nb_vfilters);
    if (ret < 0) {
        return ret;
    }

    vfilters_list[nb_vfilters - 1] = av_strdup(arg);
    if (!vfilters_list[nb_vfilters - 1]) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int opt_rotate(void *optctx av_unused, const char *opt av_unused,
                      const char *arg) {
    char *tail = NULL;
    long deg;

    errno = 0;
    deg = strtol(arg, &tail, 10);
    if (errno || tail == arg || (tail && *tail)) {
        return AVERROR(EINVAL);
    }
    if (deg % 90 != 0) {
        av_log(NULL, AV_LOG_FATAL, "-rotate must be a multiple of 90 degrees.\n");
        return AVERROR(EINVAL);
    }

    video_rotate = (int)(((deg % 360) + 360) % 360);

    return 0;
}

static int opt_width(void *optctx av_unused, const char *opt, const char *arg) {
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0) {
        return ret;
    }
    cmd_width = num;

    return 0;
}

static int opt_height(void *optctx av_unused, const char *opt, const char *arg) {
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0) {
        return ret;
    }
    cmd_height = num;

    return 0;
}

static int opt_format(void *optctx av_unused, const char *opt av_unused, const char *arg) {
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        return AVERROR(EINVAL);
    }

    return 0;
}

static int opt_sync(void *optctx av_unused, const char *opt, const char *arg) {
    if (!strcmp(arg, "audio")) {
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    } else if (!strcmp(arg, "video")) {
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    } else if (!strcmp(arg, "ext")) {
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    } else {
        fatal_quit("Unknown value for %s: %s.\n", opt, arg);
    }
    av_sync_type_explicit = 1;

    return 0;
}

static int opt_codec(void *optctx av_unused, const char *opt, const char *arg) {
    const char *spec = strchr(opt, ':');
    const char **name;
    if (!spec) {
        return AVERROR(EINVAL);
    }
    spec++;

    switch (spec[0]) {
    case 'a':
        name = &audio_codec_name;
        break;
    case 's':
        name = &subtitle_codec_name;
        break;
    case 'v':
        name = &video_codec_name;
        break;
    default:
        return AVERROR(EINVAL);
    }

    av_freep(name);
    *name = av_strdup(arg);

    return *name ? 0 : AVERROR(ENOMEM);
}

static int dummy;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS /* Just a comment to make clang-format ignore this line. */
    {"v", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_version}, "show version"},
    {"version", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_version}, "show version"},
    {"quiet", OPT_TYPE_FUNC, 0, {.func_arg = opt_quiet}, "silence all logging (overrides -loglevel)"},
    {"x", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
    {"y", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
    {"fs", OPT_TYPE_BOOL, 0, {&is_fullscreen}, "force fullscreen"},
    {"windowed", OPT_TYPE_BOOL, 0, {&start_windowed}, "start windowed instead of fullscreen"},
    {"autofit", OPT_TYPE_FLOAT, 0, {&autofit_larger}, "limit windowed size to this fraction of the display (default 0.85)", "fraction"},
    {"an", OPT_TYPE_BOOL, 0, {&audio_disable}, "disable audio"},
    {"vn", OPT_TYPE_BOOL, 0, {&video_disable}, "disable video"},
    {"sn", OPT_TYPE_BOOL, 0, {&subtitle_disable}, "disable subtitling"},
    {"ast", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]}, "select the desired audio stream", "stream_specifier"},
    {"vst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]}, "select the desired video stream", "stream_specifier"},
    {"sst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]}, "select the desired subtitle stream", "stream_specifier"},
    {"ss", OPT_TYPE_TIME, 0, {&start_time}, "seek to a given position in seconds", "pos"},
    {"t", OPT_TYPE_TIME, 0, {&play_duration}, "play this duration of the input in seconds", "duration"},
    {"bytes", OPT_TYPE_INT, 0, {&seek_by_bytes}, "seek by bytes (0 = off, 1 = on, -1 = auto)", "val"},
    {"seek_interval", OPT_TYPE_FLOAT, 0, {&seek_interval}, "set the seek interval in seconds for the left and right keys", "seconds"},
    {"nodisp", OPT_TYPE_BOOL, 0, {&display_disable}, "disable graphical display"},
    {"benchmark", OPT_TYPE_BOOL, 0, {&benchmark}, "blaze it (for benchmarking)", ""},
    {"noborder", OPT_TYPE_BOOL, 0, {&borderless}, "enable borderless window mode"},
    {"alwaysontop", OPT_TYPE_BOOL, 0, {&alwaysontop}, "try to keep the window always on top"},
    {"volume", OPT_TYPE_INT, 0, {&startup_volume}, "set the startup volume in percent (up to 260)", "volume"},
    {"mute", OPT_TYPE_BOOL, 0, {&global_muted}, "mute audio at startup"},
    {"f", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_format}, "force a format", "fmt"},
    {"fast", OPT_TYPE_BOOL, 0, {&fast}, "enable non-compliant optimizations", ""},
    {"genpts", OPT_TYPE_BOOL, 0, {&genpts}, "generate PTS", ""},
    {"drp", OPT_TYPE_INT, 0, {&decoder_reorder_pts}, "let the decoder reorder PTS (0 = off, 1 = on, -1 = auto)", ""},
    {"sync", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_sync}, "set the audio-video sync type (audio, video, ext)", "type"},
    {"skip-to-keyframe", OPT_TYPE_BOOL, 0, {&skip_to_keyframe}, "skip the video forward to keyframes instead of slowing down (drops content)", ""},
    {"no-shader-cache", OPT_TYPE_BOOL, 0, {&no_shader_cache}, "disable caching compiled shaders on disk", ""},
    {"shader-cache-dir", OPT_TYPE_STRING, 0, {&shader_cache_dir}, "directory for the shader cache", "dir"},
    {"keep-open", OPT_TYPE_BOOL, 0, {&keep_open}, "keep the window open at the end of the playlist", ""},
    {"shuffle", OPT_TYPE_BOOL, 0, {&shuffle}, "play the playlist entries in random order", ""},
    {"reverse-playlist", OPT_TYPE_BOOL, 0, {&reverse_playlist}, "play the playlist entries in reverse order", ""},
    {"pause", OPT_TYPE_BOOL, 0, {&start_paused}, "start paused on the first frame of each entry", ""},
    {"exitonkeydown", OPT_TYPE_BOOL, 0, {&exit_on_keydown}, "exit on key down", ""},
    {"exitonmousedown", OPT_TYPE_BOOL, 0, {&exit_on_mousedown}, "exit on mouse down", ""},
    {"loop", OPT_TYPE_INT, 0, {&loop}, "set the number of times playback will be looped", "loop count"},
    {"framedrop", OPT_TYPE_BOOL, 0, {&framedrop}, "drop frames when the CPU is too slow", ""},
    {"infbuf", OPT_TYPE_BOOL, 0, {&infinite_buffer}, "don't limit the input buffer size (useful with realtime streams)", ""},
    {"cache-secs", OPT_TYPE_FLOAT, 0, {&opt_cache_secs}, "stream readahead in seconds (-1 = auto: 30 for network, 1 for local)", "seconds"},
    {"cache-size", OPT_TYPE_INT, 0, {&opt_cache_size_mb}, "max readahead buffer in MB (-1 = auto: 128 for network, 15 for local)", "MB"},
    {"window_title", OPT_TYPE_STRING, 0, {&window_title}, "override the window title", "window title"},
    {"left", OPT_TYPE_INT, 0, {&screen_left}, "set the x position for the left of the window", "x pos"},
    {"top", OPT_TYPE_INT, 0, {&screen_top}, "set the y position for the top of the window", "y pos"},
    {"vf", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
    {"af", OPT_TYPE_STRING, 0, {&afilters_opt}, "set audio filters", "filter_graph"},
    {"i", OPT_TYPE_BOOL, 0, {&dummy}, "play the specified input", "input_file"},
    {"codec", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_codec}, "force a decoder", "decoder_name"},
    {"acodec", OPT_TYPE_STRING, 0, {&audio_codec_name}, "force an audio decoder", "decoder_name"},
    {"scodec", OPT_TYPE_STRING, 0, {&subtitle_codec_name}, "force a subtitle decoder", "decoder_name"},
    {"vcodec", OPT_TYPE_STRING, 0, {&video_codec_name}, "force a video decoder", "decoder_name"},
    {"autorotate", OPT_TYPE_BOOL, 0, {&autorotate}, "automatically rotate video", ""},
    {"rotate", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_rotate}, "rotate clockwise by multiples of 90 degrees", "degrees"},
    {"find_stream_info", OPT_TYPE_BOOL, OPT_INPUT, {&find_stream_info}, "read and decode the stream(s) to fill missing information with heuristics"},
    {"filter_threads", OPT_TYPE_INT, 0, {&filter_nbthreads}, "number of filter threads per graph"},
    {"enable_vulkan", OPT_TYPE_BOOL, 0, {&enable_vulkan}, "enable the Vulkan renderer"},
    {"no-vulkan", OPT_TYPE_BOOL, 0, {&disable_vulkan}, "disable the Vulkan renderer"},
    {"vulkan_params", OPT_TYPE_STRING, 0, {&vulkan_params}, "Vulkan configuration using a list of key=value pairs separated by ':'"},
    {"vulkan-swap-mode", OPT_TYPE_STRING, 0, {&vulkan_swap_mode}, "Vulkan present mode (fifo, fifo-relaxed, mailbox, immediate)", "mode"},
    {"icc-profile", OPT_TYPE_STRING, 0, {&icc_profile}, "ICC profile passed to libplacebo", "path"},
    {"video_bg", OPT_TYPE_STRING, 0, {&video_background}, "set the video background for transparent content"},
    {"hwaccel", OPT_TYPE_STRING, 0, {&hwaccel}, "use hardware accelerated decoding with the specified method, or no, or none"},
    {"no-hwaccel", OPT_TYPE_BOOL, 0, {&no_hwaccel}, "disable hardware accelerated decoding (force software)"},
    {"video_unscaled", OPT_TYPE_BOOL, 0, {&video_unscaled}, "display video at native size and scale down only if the input is too large for the display"},
    {"360-sbs", OPT_TYPE_BOOL, 0, {&enable_360sbs}, "enable 360\xc2\xb0 equirectangular projection for side-by-side video"},
    {"360-tb", OPT_TYPE_BOOL, 0, {&enable_360tb}, "enable 360\xc2\xb0 equirectangular projection for top-bottom video"},
    {"no-ytdl", OPT_TYPE_BOOL, 0, {&ytdl_disable}, "disable yt-dlp integration"},
    {"ytdl-path", OPT_TYPE_STRING, 0, {&ytdl_path}, "path to the yt-dlp binary", "path"},
    {"ytdl-format", OPT_TYPE_STRING, 0, {&ytdl_format}, "yt-dlp format selection string", "format"},
    {"delete", OPT_TYPE_BOOL, 0, {&allow_delete}, "enable permanent file deletion"},
    {"no-terminal-quit", OPT_TYPE_BOOL, 0, {&terminal_quit_disable}, "disable the terminal quit keybinding", ""},
    {
        NULL,
    },
};
#pragma GCC diagnostic pop

/* clang-format off */
#define PRINT_LIB_VERSION(libname, LIBNAME)                                       \
    av_log(NULL, AV_LOG_INFO, "  lib%-11s %2d.%3d.%3d / %2d.%3d.%3d\n", #libname, \
           LIB##LIBNAME##_VERSION_MAJOR, LIB##LIBNAME##_VERSION_MINOR,            \
           LIB##LIBNAME##_VERSION_MICRO, AV_VERSION_MAJOR(libname##_version()),   \
           AV_VERSION_MINOR(libname##_version()), AV_VERSION_MICRO(libname##_version()))
/* clang-format on */

int opt_version(void *optctx av_unused, const char *opt av_unused,
                const char *arg av_unused) {
    int this_year = program_birth_year;
    time_t t = time(NULL);

    /* Make sure the banner is visible regardless of loglevel. */
    if (av_log_get_level() < AV_LOG_INFO) {
        av_log_set_level(AV_LOG_INFO);
    }

    struct tm *tm = localtime(&t);
    if (tm && tm->tm_year + 1900 > this_year) {
        this_year = tm->tm_year + 1900;
    }

    av_log(NULL, AV_LOG_INFO,
           "%s %s, a fork of ffplay\n"
           "Copyright © %d-%d Fabrice Bellard, and the FFmpeg authors\n"
           "Copyright © %d dancingmirrors\n",
           program_name, VERSION, program_birth_year, this_year, this_year);
    av_log(NULL, AV_LOG_INFO,
           "Special thanks to the mpv and VLC authors.\n");

    av_log(NULL, AV_LOG_INFO, "FFmpeg configuration: %s\n", avutil_configuration());
    PRINT_LIB_VERSION(avutil, AVUTIL);
    PRINT_LIB_VERSION(avcodec, AVCODEC);
    PRINT_LIB_VERSION(avformat, AVFORMAT);
#if LACHESIS_HAVE_AVDEVICE
    PRINT_LIB_VERSION(avdevice, AVDEVICE);
#endif
    PRINT_LIB_VERSION(avfilter, AVFILTER);
    PRINT_LIB_VERSION(swscale, SWSCALE);
    PRINT_LIB_VERSION(swresample, SWRESAMPLE);
#if LACHESIS_HAVE_LIBPLACEBO
    av_log(NULL, AV_LOG_INFO, "  lib%-11s %s\n", "placebo", PL_VERSION);
#endif

    return 0;
}

void show_help_default(const char *opt av_unused, const char *arg av_unused) {
    show_help_options(options, 0, 0);
}
