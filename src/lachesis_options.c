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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/eval.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/parseutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

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
int keep_open;
int shuffle;
int reverse_playlist;
int start_paused;
int loop = 1;
float opt_cache_secs = -1.0f;
int opt_cache_size_mb = -1;
const char *audio_codec_name;
const char *subtitle_codec_name;
const char *video_codec_name;
const char **vfilters_list = NULL;
int nb_vfilters = 0;
char *afilters_opt = NULL;
int autorotate = 1;
int disable_autorotate = 0;
int video_rotate = 0;
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

static int grow_array(void **array, int elem_size, int *size, int new_size) {
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        return AVERROR(ERANGE);
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(*array, new_size, elem_size);
        if (!tmp) {
            return AVERROR(ENOMEM);
        }
        memset(tmp + *size * elem_size, 0, (new_size - *size) * elem_size);
        *size = new_size;
        *array = tmp;
        return 0;
    }

    return 0;
}

#define GROW_ARRAY(array, nb_elems) \
    grow_array((void **)&array, sizeof(*array), &nb_elems, nb_elems + 1)

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS /* Just a comment to make clang-format ignore this line. */
    {"v", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_version}, "show version"},
    {"version", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_version}, "show version"},
    {"quiet", OPT_TYPE_FUNC, 0, {.func_arg = opt_quiet}, "silence all logging (overrides -loglevel)"},
    {"x", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
    {"y", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
    {"windowed", OPT_TYPE_BOOL, 0, {&start_windowed}, "start windowed instead of fullscreen"},
    {"autofit", OPT_TYPE_FLOAT, 0, {&autofit_larger}, "limit windowed size to this fraction of the display (default 0.85)", "fraction"},
    {"an", OPT_TYPE_BOOL, 0, {&audio_disable}, "disable audio"},
    {"vn", OPT_TYPE_BOOL, 0, {&video_disable}, "disable video"},
    {"sn", OPT_TYPE_BOOL, 0, {&subtitle_disable}, "disable subtitles"},
    {"ast", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]}, "select the desired audio stream", "stream_specifier"},
    {"vst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]}, "select the desired video stream", "stream_specifier"},
    {"sst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]}, "select the desired subtitle stream", "stream_specifier"},
    {"ss", OPT_TYPE_TIME, 0, {&start_time}, "seek to a given position in seconds", "pos"},
    {"t", OPT_TYPE_TIME, 0, {&play_duration}, "play this duration of the input in seconds", "duration"},
    {"seek_interval", OPT_TYPE_FLOAT, 0, {&seek_interval}, "set the seek interval in seconds for the left and right keys", "seconds"},
    {"nodisp", OPT_TYPE_BOOL, 0, {&display_disable}, "disable graphical display"},
    {"benchmark", OPT_TYPE_BOOL, 0, {&benchmark}, "blaze it (for benchmarking)", ""},
    {"noborder", OPT_TYPE_BOOL, 0, {&borderless}, "enable borderless window mode"},
    {"alwaysontop", OPT_TYPE_BOOL, 0, {&alwaysontop}, "try to keep the window always on top"},
    {"volume", OPT_TYPE_INT, 0, {&startup_volume}, "set the startup volume in percent (up to 260)", "volume"},
    {"mute", OPT_TYPE_BOOL, 0, {&global_muted}, "mute audio at startup"},
    {"f", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_format}, "force a format", "fmt"},
    {"sync", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_sync}, "set the audio-video sync type (audio, video, ext)", "type"},
    {"skip-to-keyframe", OPT_TYPE_BOOL, 0, {&skip_to_keyframe}, "skip the video forward to keyframes instead of slowing down (drops content)", ""},
    {"no-shader-cache", OPT_TYPE_BOOL, 0, {&no_shader_cache}, "disable caching compiled shaders on disk", ""},
    {"shader-cache-dir", OPT_TYPE_STRING, 0, {&shader_cache_dir}, "directory for the shader cache", "dir"},
    {"keep-open", OPT_TYPE_BOOL, 0, {&keep_open}, "keep the window open at the end of the playlist", ""},
    {"shuffle", OPT_TYPE_BOOL, 0, {&shuffle}, "play the playlist entries in random order", ""},
    {"reverse-playlist", OPT_TYPE_BOOL, 0, {&reverse_playlist}, "play the playlist entries in reverse order", ""},
    {"pause", OPT_TYPE_BOOL, 0, {&start_paused}, "start paused on the first frame of each entry", ""},
    {"loop", OPT_TYPE_INT, 0, {&loop}, "set the number of times playback will be looped", "loop count"},
    {"cache-secs", OPT_TYPE_FLOAT, 0, {&opt_cache_secs}, "stream readahead in seconds (-1 = auto: 30 for network, 1 for local)", "seconds"},
    {"cache-size", OPT_TYPE_INT, 0, {&opt_cache_size_mb}, "max readahead buffer in MB (-1 = auto: 128 for network, 15 for local)", "MB"},
    {"window_title", OPT_TYPE_STRING, 0, {&window_title}, "override the window title", "window title"},
    {"left", OPT_TYPE_INT, 0, {&screen_left}, "set the x position for the left of the window", "x pos"},
    {"top", OPT_TYPE_INT, 0, {&screen_top}, "set the y position for the top of the window", "y pos"},
    {"vf", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
    {"af", OPT_TYPE_STRING, 0, {&afilters_opt}, "set audio filters", "filter_graph"},
    {"acodec", OPT_TYPE_STRING, 0, {&audio_codec_name}, "force an audio decoder", "decoder_name"},
    {"scodec", OPT_TYPE_STRING, 0, {&subtitle_codec_name}, "force a subtitle decoder", "decoder_name"},
    {"vcodec", OPT_TYPE_STRING, 0, {&video_codec_name}, "force a video decoder", "decoder_name"},
    {"no-autorotate", OPT_TYPE_BOOL, 0, {&disable_autorotate}, "disable automatic rotation", ""},
    {"rotate", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_rotate}, "rotate clockwise by multiples of 90 degrees", "degrees"},
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

void show_help_default(void) {
    show_help_options(options);
}

int parse_number(const char *context, const char *numstr, enum OptionType type,
                 double min, double max, double *dst) {
    char *tail;
    const char *error;
    double d = av_strtod(numstr, &tail);
    if (*tail) {
        error = "Expected number for %s but found: %s\n";
    } else if (d < min || d > max) {
        error = "The value for %s was %s which is not within %f - %f\n";
    } else if (type == OPT_TYPE_INT64 && (int64_t)d != d) {
        error = "Expected int64 for %s but found %s\n";
    } else if (type == OPT_TYPE_INT && (int)d != d) {
        error = "Expected int for %s but found %s\n";
    } else {
        *dst = d;
        return 0;
    }
    av_log(NULL, AV_LOG_FATAL, error, context, numstr, min, max);

    return AVERROR(EINVAL);
}

void show_help_options(const OptionDef *defs) {
    const OptionDef *po;
    int max_width = 0;

    for (po = defs; po->name; po++) {
        int width = strlen(po->name) + 1; /* +1 for the leading '-' */
        if (po->argname) {
            width += strlen(po->argname) + 3; /* +3 for " <>" */
        }
        if (width > max_width) {
            max_width = width;
        }
    }

    for (po = defs; po->name; po++) {
        char buf[128];

        av_strlcpy(buf, po->name, sizeof(buf));

        if (po->argname) {
            av_strlcatf(buf, sizeof(buf), " <%s>", po->argname);
        }

        printf("-%-*s  %s\n", max_width, buf, po->help);
    }
}

int opt_help(void *optctx av_unused, const char *opt av_unused,
             const char *arg av_unused) {
    show_help_default();
    return 0;
}

static const OptionDef *find_option(const OptionDef *po, const char *name) {
    while (po->name) {
        const char *end;
        if (av_strstart(name, po->name, &end) && (!*end || *end == ':')) {
            break;
        }
        po++;
    }
    return po;
}

static int opt_has_arg(const OptionDef *o) {
    if (o->type == OPT_TYPE_BOOL) {
        return 0;
    }
    if (o->type == OPT_TYPE_FUNC) {
        return !!(o->flags & OPT_FUNC_ARG);
    }
    return 1;
}

static int write_option(void *optctx, const OptionDef *po, const char *opt,
                        const char *arg) {
    void *dst = po->u.dst_ptr;
    double num;
    int ret = 0;

    if (po->type == OPT_TYPE_STRING) {
        char *str = av_strdup(arg);
        av_freep(dst);
        if (!str) {
            return AVERROR(ENOMEM);
        }
        *(char **)dst = str;
    } else if (po->type == OPT_TYPE_BOOL || po->type == OPT_TYPE_INT) {
        ret = parse_number(opt, arg, OPT_TYPE_INT64, INT_MIN, INT_MAX, &num);
        if (ret < 0) {
            return ret;
        }
        *(int *)dst = num;
    } else if (po->type == OPT_TYPE_INT64) {
        ret = parse_number(opt, arg, OPT_TYPE_INT64, INT64_MIN, (double)INT64_MAX, &num);
        if (ret < 0) {
            return ret;
        }
        *(int64_t *)dst = num;
    } else if (po->type == OPT_TYPE_TIME) {
        ret = av_parse_time(dst, arg, 1);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Invalid duration for option %s: %s\n", opt, arg);
            return ret;
        }
    } else if (po->type == OPT_TYPE_FLOAT) {
        ret = parse_number(opt, arg, OPT_TYPE_FLOAT, -INFINITY, INFINITY, &num);
        if (ret < 0) {
            return ret;
        }
        *(float *)dst = num;
    } else if (po->type == OPT_TYPE_DOUBLE) {
        ret = parse_number(opt, arg, OPT_TYPE_DOUBLE, -INFINITY, INFINITY, &num);
        if (ret < 0) {
            return ret;
        }
        *(double *)dst = num;
    } else {
        av_assert0(po->type == OPT_TYPE_FUNC && po->u.func_arg);
        ret = po->u.func_arg(optctx, opt, arg);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to set value '%s' for option '%s': %s\n",
                   arg, opt, av_err2str(ret));
            return ret;
        }
    }
    if (po->flags & OPT_EXIT) {
        return AVERROR_EXIT;
    }
    return 0;
}

int parse_option(void *optctx, const char *opt, const char *arg,
                 const OptionDef *defs) {
    const OptionDef *po;
    int ret;

    po = find_option(defs, opt);
    if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
        /* handle 'no' bool option */
        po = find_option(defs, opt + 2);
        if (po->name && po->type == OPT_TYPE_BOOL) {
            arg = "0";
        }
    } else if (po->type == OPT_TYPE_BOOL) {
        arg = "1";
    }

    if (!po->name) {
        av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'\n", opt);
        return AVERROR(EINVAL);
    }
    if (opt_has_arg(po) && !arg) {
        av_log(NULL, AV_LOG_ERROR, "Missing argument for option '%s'\n", opt);
        return AVERROR(EINVAL);
    }

    ret = write_option(optctx, po, opt, arg);
    if (ret < 0) {
        return ret;
    }

    return opt_has_arg(po);
}

static int config_parse_bool(const char *val) {
    static const char *const yes[] = {"1", "yes", "true", "on", "enable", "enabled"};
    static const char *const no[] = {"0", "no", "false", "off", "disable", "disabled"};

    for (size_t i = 0; i < FF_ARRAY_ELEMS(yes); i++) {
        if (!av_strcasecmp(val, yes[i])) {
            return 1;
        }
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(no); i++) {
        if (!av_strcasecmp(val, no[i])) {
            return 0;
        }
    }
    return -1;
}

int parse_config_option(void *optctx, const char *opt, const char *arg,
                        const OptionDef *defs, const char *src) {
    const OptionDef *po = find_option(defs, opt);
    int ret;

    if (!po->name) {
        log_warn("%s: unknown option '%s', ignoring.\n", src, opt);
        return AVERROR(EINVAL);
    }
    /* For example --help, --version... */
    if (po->flags & OPT_EXIT) {
        log_warn("%s: option '%s' is not allowed, ignoring.\n",
                 src, opt);
        return AVERROR(EINVAL);
    }

    if (po->type == OPT_TYPE_BOOL) {
        int on = config_parse_bool(arg);
        if (on < 0) {
            log_warn("%s: option '%s' wants yes or no, got '%s', ignoring.\n",
                     src, opt, arg);
            return AVERROR(EINVAL);
        }
        arg = on ? "1" : "0";
    } else if (!opt_has_arg(po)) {
        int on = config_parse_bool(arg);
        if (on < 0) {
            log_warn("%s: option '%s' wants yes or no, got '%s', ignoring.\n",
                     src, opt, arg);
            return AVERROR(EINVAL);
        }
        if (!on) {
            return 0;
        }
    } else if (po->type != OPT_TYPE_STRING && (!arg || !arg[0])) {
        /* "" is allowed. */
        log_warn("%s: option '%s' needs a value, ignoring.\n", src, opt);
        return AVERROR(EINVAL);
    }

    ret = write_option(optctx, po, opt, arg);
    if (ret < 0 && ret != AVERROR_EXIT) {
        log_warn("%s: could not apply option '%s'.\n", src, opt);
    }
    return ret;
}

int parse_options(void *optctx, int argc, char **argv, const OptionDef *defs,
                  int (*parse_arg_function)(void *, const char *)) {
    const char *opt;
    int optindex, handleoptions = 1, ret;

    optindex = 1;
    while (optindex < argc) {
        opt = argv[optindex++];

        if (handleoptions && opt[0] == '-' && opt[1] != '\0') {
            if (opt[1] == '-' && opt[2] == '\0') {
                handleoptions = 0;
                continue;
            }
            opt++;

            if (opt[0] == '-' && opt[1] != '\0') {
                opt++;
            }

            if ((ret = parse_option(optctx, opt, argv[optindex], defs)) < 0) {
                return ret;
            }
            optindex += ret;
        } else {
            if (parse_arg_function) {
                ret = parse_arg_function(optctx, opt);
                if (ret < 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

static int locate_option(int argc, char **argv, const OptionDef *defs,
                         const char *optname) {
    const OptionDef *po;
    int i;

    for (i = 1; i < argc; i++) {
        const char *cur_opt = argv[i];

        if (!(cur_opt[0] == '-' && cur_opt[1])) {
            continue;
        }
        cur_opt++;

        if (cur_opt[0] == '-' && cur_opt[1] != '\0') {
            cur_opt++;
        }

        po = find_option(defs, cur_opt);
        if (!po->name && cur_opt[0] == 'n' && cur_opt[1] == 'o') {
            po = find_option(defs, cur_opt + 2);
        }

        if ((!po->name && !strcmp(cur_opt, optname)) ||
            (po->name && !strcmp(optname, po->name))) {
            return i;
        }

        if (!po->name || opt_has_arg(po)) {
            i++;
        }
    }

    return 0;
}

static const struct {
    const char *name;
    int level;
} log_levels[] = {
    {"quiet", AV_LOG_QUIET},
    {"panic", AV_LOG_PANIC},
    {"fatal", AV_LOG_FATAL},
    {"error", AV_LOG_ERROR},
    {"warning", AV_LOG_WARNING},
    {"info", AV_LOG_INFO},
    {"verbose", AV_LOG_VERBOSE},
    {"debug", AV_LOG_DEBUG},
    {"trace", AV_LOG_TRACE},
};

int opt_loglevel(void *optctx av_unused, const char *opt av_unused, const char *arg) {
    const char *token;
    char *tail;
    int flags = av_log_get_flags();
    int level = av_log_get_level();
    int cmd;
    size_t i = 0;

    /* Optional leading "repeat+"/"+repeat" flags, '+'-separated from the
     * level. */
    while (*arg) {
        token = arg;
        if (*token == '+' || *token == '-') {
            cmd = *token;
            token++;
        } else {
            cmd = 0;
        }

        if (!i && !strncmp(token, "repeat", 6)) {
            if (cmd == '-') {
                flags |= AV_LOG_SKIP_REPEATED;
            } else {
                flags &= ~AV_LOG_SKIP_REPEATED;
            }
            av_log_set_flags(flags);
            arg = token + 6;
        } else {
            break;
        }
        i++;
        while (*arg == '+') {
            arg++;
        }
    }

    if (*arg) {
        for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
            if (!strcmp(log_levels[i].name, arg)) {
                level = log_levels[i].level;
                goto end;
            }
        }
        level = strtol(arg, &tail, 10);
        if (*tail) {
            av_log(NULL, AV_LOG_FATAL, "Invalid loglevel \"%s\". "
                                       "Possible levels are numbers or:\n",
                   arg);
            for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
                av_log(NULL, AV_LOG_FATAL, "\"%s\"\n", log_levels[i].name);
            }
            return AVERROR(EINVAL);
        }
    }

end:
    if (!lachesis_quiet) {
        av_log_set_level(level);
    }

    return 0;
}

void parse_loglevel(int argc, char **argv, const OptionDef *defs) {
    int idx = locate_option(argc, argv, defs, "loglevel");
    if (idx && argv[idx + 1]) {
        opt_loglevel(NULL, "loglevel", argv[idx + 1]);
    }
}

int opt_quiet(void *optctx av_unused, const char *opt av_unused,
              const char *arg av_unused) {
    lachesis_quiet = 1;
    av_log_set_level(AV_LOG_QUIET);

    return 0;
}

void parse_quiet(int argc, char **argv, const OptionDef *defs) {
    if (locate_option(argc, argv, defs, "quiet")) {
        opt_quiet(NULL, "quiet", NULL);
    }
}
