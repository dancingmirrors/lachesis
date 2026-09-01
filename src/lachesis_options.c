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

#include <libplacebo/config.h>

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
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/eval.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/parseutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "lachesis_alloc.h"
#include "lachesis_audio.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_playlist.h"
#include "lachesis_scale.h"
#include "lachesis_single.h"

const AVInputFormat *file_iformat;
const char *window_title;
char *window_title_auto;
int audio_disable;
int video_disable;
int subtitle_disable;
const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
float seek_interval = 5.0;
int display_disable;
int benchmark;
int alwaysontop;
int startup_volume = 100;
int av_sync_type = AV_SYNC_AUDIO_MASTER;
int av_sync_type_explicit = 0;
int slow = 0;
int no_edit_list = 0;
int64_t start_time = AV_NOPTS_VALUE;
int64_t play_duration = AV_NOPTS_VALUE;
int64_t sub_offset = AV_NOPTS_VALUE;
int keep_open;
int archive_jump_last;
int allow_unsafe;
int all_files;
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
const char *audio_spdif_opt = NULL;
int audio_spdif_force = 0;
int autorotate = 1;
int disable_autorotate = 0;
int video_rotate = 0;
enum RendererApi gpu_api = RENDERER_API_AUTO;
char *gpu_api_name = NULL;
int no_vulkan = 0;
char *gpu_params = NULL;
char *vulkan_swap_mode = NULL;
int max_glsl_version = 0;
int no_shader_cache = 0;
char *shader_cache_dir = NULL;
const char *icc_profile = NULL;
int icc_auto = 0;
int no_display_hdr = 0;
char *video_background = NULL;
const char *hwaccel = NULL;
int no_hwaccel = 0;
const char *hwaccel_codecs = NULL;
int hwaccel_max_size = 0;
int max_texture_size = 0;
int video_fill = 0;
int enable_360sbs = 0;
int enable_360tb = 0;
int enable_360eq = 0;
int enable_360eqtb = 0;
int is_fullscreen = 1;
int start_windowed = 0;
int window_resize = 0;
float autofit_larger = 0.85f;
int global_muted = 0;
int ytdl_disable = 0;
const char *ytdl_path = NULL;
const char *ytdl_format = NULL;
int allow_delete = 0;
int terminal_quit_disable = 0;
double display_fps_override = 0.0;
int no_vsync_snap = 0;
double fps_convert = 0.0;
int allow_volume_boost = 1;
int normalize_audio = 0;
double normalize_target = -23.0;
double normalize_gain = 2.0;

static int grow_array(void **array, int elem_size, int *size, int new_size) {
    if (new_size >= INT_MAX / elem_size) {
        log_dead("Array too big.\n");
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

static const char *option_value_list(const char *const *values, const char *conj,
                                     char *buf, size_t size) {
    size_t n = 0;

    while (values[n]) {
        n++;
    }
    buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (!i) {
        } else if (!conj || i + 1 < n) {
            av_strlcat(buf, ", ", size);
        } else {
            av_strlcatf(buf, size, "%s%s ", n > 2 ? ", " : " ", conj);
        }
        av_strlcat(buf, values[i], size);
    }

    return buf;
}

static int opt_value_index(const char *arg, const char *const *values) {
    for (int i = 0; values[i]; i++) {
        if (!strcmp(arg, values[i])) {
            return i;
        }
    }

    return -1;
}

static int opt_bad_value(const char *opt, const char *arg,
                         const char *const *values) {
    char list[256];

    log_dead("-%s must be %s, not '%s'.\n", opt,
             option_value_list(values, "or", list, sizeof(list)), arg);

    return AVERROR(EINVAL);
}

static const char *const edit_list_modes[] = {"auto", "off", NULL};
static const char *const archive_jump_modes[] = {"first", "last", NULL};
static const char *const sync_types[] = {"audio", "video", "ext", NULL};
static const char *const swap_modes[] = {"fifo", "fifo-relaxed", "mailbox",
                                         "immediate", NULL};

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

static int opt_rotate(void *optctx av_unused, const char *opt, const char *arg) {
    char *tail = NULL;
    long deg;

    errno = 0;
    deg = strtol(arg, &tail, 10);
    if (errno || tail == arg || (tail && *tail)) {
        log_dead("-%s wants a number of degrees, not '%s'.\n", opt, arg);
        return AVERROR(EINVAL);
    }
    if (deg % 90 != 0) {
        log_dead("-%s must be a multiple of 90 degrees.\n", opt);
        return AVERROR(EINVAL);
    }

    video_rotate = (int)(((deg % 360) + 360) % 360);

    return 0;
}

static int opt_supersample(void *optctx av_unused, const char *opt,
                           const char *arg) {
    if (opt_value_index(arg, supersample_level_names) < 0) {
        return opt_bad_value(opt, arg, supersample_level_names);
    }
    supersample_level = supersample_level_parse(arg);

    return 0;
}

static int opt_scale(void *optctx av_unused, const char *opt, const char *arg) {
    if (!strcmp(arg, "help")) {
        scale_filter_list();
        return AVERROR_EXIT;
    }
    if (!scale_filter_set(arg)) {
        log_dead("Unknown scaler '%s'. "
                 "Try -%s help.\n",
                 arg, opt);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int opt_archive_jump(void *optctx av_unused, const char *opt,
                            const char *arg) {
    int i = opt_value_index(arg, archive_jump_modes);

    if (i < 0) {
        return opt_bad_value(opt, arg, archive_jump_modes);
    }
    archive_jump_last = i;

    return 0;
}

static int opt_edit_list(void *optctx av_unused, const char *opt,
                         const char *arg) {
    int i = opt_value_index(arg, edit_list_modes);

    if (i < 0) {
        return opt_bad_value(opt, arg, edit_list_modes);
    }
    no_edit_list = i;

    return 0;
}

static int opt_single(void *optctx av_unused, const char *opt, const char *arg) {
    int i = opt_value_index(arg, single_modes);

    if (i < 0) {
        return opt_bad_value(opt, arg, single_modes);
    }
    single_mode = i;

    return 0;
}

static int opt_loop(void *optctx av_unused, const char *opt, const char *arg) {
    double count;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, INT_MIN, INT_MAX, &count);

    if (ret < 0) {
        return ret;
    }
    if (count < 0) {
        log_dead("-%s must not be a negative value.\n", opt);
        return AVERROR(EINVAL);
    }
    loop = (int)count;

    return 0;
}

static int store_string(const char **dst, const char *arg) {
    char *str = av_strdup(arg);

    if (!str) {
        return AVERROR(ENOMEM);
    }
    av_freep(dst);
    *dst = str;

    return 0;
}

static int opt_vulkan_swap_mode(void *optctx av_unused, const char *opt,
                                const char *arg) {
    if (opt_value_index(arg, swap_modes) < 0) {
        return opt_bad_value(opt, arg, swap_modes);
    }

    return store_string((const char **)&vulkan_swap_mode, arg);
}

static const char *hwaccel_method_list(char *buf, size_t size) {
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

    buf[0] = '\0';
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        const char *name = av_hwdevice_get_type_name(type);
        size_t len = strlen(buf);

        if (!name) {
            continue;
        }
        if (len && av_strlcat(buf, ", ", size) >= size) {
            buf[len] = '\0';
            return buf;
        }
        if (av_strlcat(buf, name, size) >= size) {
            buf[len] = '\0';
            return buf;
        }
    }

    return buf;
}

static int hwaccel_off_name(const char *arg) {
    return !strcmp(arg, "no") || !strcmp(arg, "none") || !strcmp(arg, "off") ||
        !strcmp(arg, "0");
}

static int opt_hwaccel(void *optctx av_unused, const char *opt av_unused,
                       const char *arg) {
    char methods[256];
    int ret;

    if (hwaccel_off_name(arg)) {
        av_freep(&hwaccel);
        no_hwaccel = 1;
        return 0;
    }
    if (av_hwdevice_find_type_by_name(arg) == AV_HWDEVICE_TYPE_NONE) {
        hwaccel_method_list(methods, sizeof(methods));
        log_dead("Unknown hwaccel method '%s'! Available methods: %s.\n",
                 arg, *methods ? methods : "no others");
        return AVERROR(EINVAL);
    }

    ret = store_string(&hwaccel, arg);
    if (ret < 0) {
        return ret;
    }
    no_hwaccel = 0;

    return 0;
}

static int opt_decoder(const char **dst, enum AVMediaType type, const char *opt,
                       const char *arg) {
    const AVCodec *codec = avcodec_find_decoder_by_name(arg);
    const char *decodes;

    if (!codec) {
        log_dead("Unknown decoder '%s' for -%s!\n", arg, opt);
        return AVERROR(EINVAL);
    }
    if (codec->type != type) {
        decodes = av_get_media_type_string(codec->type);
        log_dead("The decoder '%s' for -%s decodes %s, not %s!\n",
                 arg, opt, decodes ? decodes : "something else",
                 av_get_media_type_string(type));
        return AVERROR(EINVAL);
    }

    return store_string(dst, arg);
}

static int opt_vcodec(void *optctx av_unused, const char *opt, const char *arg) {
    return opt_decoder(&video_codec_name, AVMEDIA_TYPE_VIDEO, opt, arg);
}

static int opt_acodec(void *optctx av_unused, const char *opt, const char *arg) {
    return opt_decoder(&audio_codec_name, AVMEDIA_TYPE_AUDIO, opt, arg);
}

static int opt_scodec(void *optctx av_unused, const char *opt, const char *arg) {
    return opt_decoder(&subtitle_codec_name, AVMEDIA_TYPE_SUBTITLE, opt, arg);
}

int parse_video_background(const char *value, uint8_t rgba[4]) {
    if (!strcmp(value, "none")) {
        return VIDEO_BACKGROUND_NONE;
    }
    if (!strcmp(value, "tiles")) {
        return VIDEO_BACKGROUND_TILES;
    }
    /* Any other value is parsed as a color. */
    if (av_parse_color(rgba, value, -1, NULL) < 0) {
        return -1;
    }

    return VIDEO_BACKGROUND_COLOR;
}

static int opt_video_bg(void *optctx av_unused, const char *opt, const char *arg) {
    uint8_t rgba[4];

    if (parse_video_background(arg, rgba) < 0) {
        log_dead("-%s must be none, tiles, or a color.\n", opt);
        return AVERROR(EINVAL);
    }

    return store_string((const char **)&video_background, arg);
}

static int opt_icc_profile(void *optctx av_unused, const char *opt av_unused,
                           const char *arg) {
    FILE *f;

    if (arg[0]) {
        f = fopen(arg, "rb");
        if (!f) {
            log_dead("Failed to open the ICC profile '%s'!\n", arg);
            return AVERROR(EINVAL);
        }
        fclose(f);
    }

    return store_string(&icc_profile, arg);
}

static int opt_gpu_params(void *optctx av_unused, const char *opt,
                          const char *arg) {
    AVDictionary *dict = NULL;
    int ret = av_dict_parse_string(&dict, arg, "=", ":", 0);

    av_dict_free(&dict);
    if (ret < 0) {
        log_dead("-%s must be key=value pairs separated by ':'.\n", opt);
        return AVERROR(EINVAL);
    }

    return store_string((const char **)&gpu_params, arg);
}

static int codec_name_known(const char *name) {
    const AVCodecDescriptor *desc = NULL;

    while ((desc = avcodec_descriptor_next(desc))) {
        if (!av_strcasecmp(desc->name, name)) {
            return 1;
        }
    }

    return 0;
}

static int opt_hwaccel_codecs(void *optctx av_unused, const char *opt,
                              const char *arg) {
    char *list = av_mallocz(strlen(arg) + 1);
    const char *p = arg;
    size_t len = 0;

    if (!list) {
        return AVERROR(ENOMEM);
    }

    while (*p) {
        const char *entry = p + strspn(p, " \t");
        size_t n = strcspn(entry, ",");
        char name[64];
        size_t skip;
        int exclude;
        int known;

        p = entry + n;
        p += *p == ',';
        while (n && (entry[n - 1] == ' ' || entry[n - 1] == '\t')) {
            n--;
        }
        if (!n) {
            continue;
        }

        exclude = entry[0] == '-';
        skip = exclude + strspn(entry + exclude, " \t");
        if (n == skip) {
            continue;
        }

        av_strlcpy(name, entry + skip, FFMIN(sizeof(name), n - skip + 1));
        known = !av_strcasecmp(name, "all") || codec_name_known(name);
        if (!known) {
            log_warn("Unknown codec '%s' for -%s.\n", name, opt);
        }

        if (len) {
            list[len++] = ',';
        }
        if (exclude) {
            list[len++] = '-';
        }
        memcpy(list + len, entry + skip, n - skip);
        len += n - skip;
    }
    list[len] = '\0';

    av_freep(&hwaccel_codecs);
    hwaccel_codecs = list;

    return 0;
}

static int opt_autofit(void *optctx av_unused, const char *opt, const char *arg) {
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_FLOAT,
                           AUTOFIT_MIN, AUTOFIT_MAX, &num);
    if (ret < 0) {
        return ret;
    }
    autofit_larger = num;

    return 0;
}

static int opt_format(void *optctx av_unused, const char *opt av_unused, const char *arg) {
    const AVInputFormat *fmt = av_find_input_format(arg);

    if (!fmt) {
        log_dead("Unknown input format '%s'!\n", arg);
        return AVERROR(EINVAL);
    }
    file_iformat = fmt;

    return 0;
}

static int opt_sync(void *optctx av_unused, const char *opt, const char *arg) {
    static const int masters[] = {AV_SYNC_AUDIO_MASTER, AV_SYNC_VIDEO_MASTER,
                                  AV_SYNC_EXTERNAL_CLOCK};
    int i = opt_value_index(arg, sync_types);

    if (i < 0) {
        return opt_bad_value(opt, arg, sync_types);
    }
    av_sync_type = masters[i];
    av_sync_type_explicit = 1;

    return 0;
}

static int arg_is_number(const char *arg) {
    char *tail;
    double num = av_strtod(arg, &tail);

    return tail != arg && !*tail && !isnan(num) && !isinf(num);
}

static int arg_is_spdif_codecs(const char *arg) {
    return audio_spdif_names_known(arg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS /* Just a comment to make clang-format ignore this line. */
    {"v", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_version}, "show version"},
    OPT_ALIAS("version", "v"),
    {"quiet", OPT_TYPE_FUNC, 0, {.func_arg = opt_quiet}, "silence all logging (overrides -loglevel)"},
    {"windowed", OPT_TYPE_BOOL, 0, {&start_windowed}, "start windowed instead of fullscreen"},
    {"autofit", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_autofit}, "limit windowed size to this fraction of the display (default 0.85)", "fraction"},
    {"resize-window", OPT_TYPE_BOOL, 0, {&window_resize}, "disable a fixed window size"},
    {"an", OPT_TYPE_BOOL, 0, {&audio_disable}, "disable audio"},
    {"vn", OPT_TYPE_BOOL, 0, {&video_disable}, "disable video"},
    {"sn", OPT_TYPE_BOOL, 0, {&subtitle_disable}, "disable subtitles"},
    {"ast", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]}, "select the desired audio stream", "stream_specifier"},
    {"vst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]}, "select the desired video stream", "stream_specifier"},
    {"sst", OPT_TYPE_STRING, 0, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]}, "select the desired subtitle stream", "stream_specifier"},
    {"ss", OPT_TYPE_TIME, 0, {&start_time}, "seek to a given position in seconds", "pos"},
    OPT_ALIAS("start", "ss"),
    {"t", OPT_TYPE_TIME, 0, {&play_duration}, "play this duration of the input in seconds", "duration"},
    OPT_ALIAS("end", "t"),
    {"seek-interval", OPT_TYPE_FLOAT, 0, {&seek_interval}, "set the seek interval in seconds for the left and right keys", "seconds"},
    {"nodisp", OPT_TYPE_BOOL, 0, {&display_disable}, "disable graphical display"},
    {"benchmark", OPT_TYPE_BOOL, 0, {&benchmark}, "blaze it (for benchmarking)"},
    {"alwaysontop", OPT_TYPE_BOOL, 0, {&alwaysontop}, "try to always keep the window on top"},
    {"volume", OPT_TYPE_INT, 0, {&startup_volume}, "set the startup volume in percent (up to 300)", "volume"},
    {"mute", OPT_TYPE_BOOL, 0, {&global_muted}, "mute audio at startup"},
    {"normalize", OPT_TYPE_BOOL, 0, {&normalize_audio}, "loudness normalization"},
    {"normalize-target", OPT_TYPE_DOUBLE, 0, {&normalize_target}, "loudness normalization target in LUFS (default -23)", "LUFS"},
    {"normalize-gain", OPT_TYPE_DOUBLE, 0, {&normalize_gain}, "extra gain over the loudness normalization in dB (default 2)", "dB"},
    {"f", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_format}, "force a format", "fmt"},
    {"edit-list", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_edit_list}, "whether to honor edit lists", "mode", edit_list_modes},
    {"sync", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_sync}, "set the audio-video sync type", "type", sync_types},
    {"slow", OPT_TYPE_BOOL, 0, {&slow}, "disable degraded decoding"},
    {"no-shader-cache", OPT_TYPE_BOOL, 0, {&no_shader_cache}, "disable caching compiled shaders on disk"},
    {"shader-cache-dir", OPT_TYPE_STRING, 0, {&shader_cache_dir}, "directory for the shader cache", "dir"},
    {"keep-open", OPT_TYPE_BOOL, 0, {&keep_open}, "keep the window open at the end of the playlist"},
    {"archive-jump", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_archive_jump}, "which end of the previous archive the comma key lands on (default first)", "where", archive_jump_modes},
    {"allow-unsafe", OPT_TYPE_BOOL, OPT_CMDLINE_ONLY, {&allow_unsafe}, "expand unsafe entries into a playlist (command line only)"},
    {"all-files", OPT_TYPE_BOOL, OPT_CMDLINE_ONLY, {&all_files}, "try to play any file in an archive or directory (command line only)"},
    {"single", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_ARG_OPTIONAL | OPT_STRICT_VALUE, {.func_arg = opt_single}, "use a single instance (yes), append to the playlist of an existing instance (queue), or neither (no)", "mode", single_modes, "yes", "no"},
    {"shuffle", OPT_TYPE_BOOL, 0, {&shuffle}, "play the playlist entries in random order"},
    {"reverse-playlist", OPT_TYPE_BOOL, 0, {&reverse_playlist}, "play the playlist entries in reverse order"},
    {"pause", OPT_TYPE_BOOL, 0, {&start_paused}, "start paused on the first frame of each entry"},
    {"loop", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_ARG_OPTIONAL, {.func_arg = opt_loop}, "set the number of times each playlist entry is played (0 or implied is forever)", "count", NULL, "0", "1", arg_is_number},
    {"cache-secs", OPT_TYPE_FLOAT, 0, {&opt_cache_secs}, "stream readahead in seconds (-1 = auto: 30 for network, 1 for local)", "seconds"},
    {"cache-size", OPT_TYPE_INT, 0, {&opt_cache_size_mb}, "max readahead buffer in MB (-1 = auto: 128 for network, 15 for local)", "MB"},
    {"window-title", OPT_TYPE_STRING, 0, {&window_title}, "override the window title", "window title"},
    {"vf", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_add_vfilter}, "set video filters (pass more than once to cycle between them with W)", "filter_graph"},
    {"af", OPT_TYPE_STRING, 0, {&afilters_opt}, "set audio filters", "filter_graph"},
    {"audio-spdif", OPT_TYPE_STRING, OPT_ARG_OPTIONAL | OPT_STRICT_VALUE, {&audio_spdif_opt}, "a list of ac3, eac3, dts, dts-hd, truehd, mp1, mp2, mp3, aac, or all separated by ',' (or implied all)", "codecs", NULL, "all", "", arg_is_spdif_codecs},
    {"audio-spdif-force", OPT_TYPE_BOOL, 0, {&audio_spdif_force}, "pass audio through even when the device format does not match"},
    {"acodec", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_acodec}, "force an audio decoder", "decoder_name"},
    {"scodec", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_scodec}, "force a subtitle decoder", "decoder_name"},
    {"sub-offset", OPT_TYPE_TIME, 0, {&sub_offset}, "shift an external subtitle by this many seconds (0 keeps its own timestamps)", "seconds"},
    {"vcodec", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_vcodec}, "force a video decoder", "decoder_name"},
    {"no-autorotate", OPT_TYPE_BOOL, 0, {&disable_autorotate}, "disable automatic rotation"},
    {"rotate", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_rotate}, "rotate clockwise by multiples of 90 degrees", "degrees"},
    {"gpu-api", OPT_TYPE_STRING, 0, {&gpu_api_name}, "GPU backend to use (auto, vulkan, opengl, d3d11)", "api"},
    {"no-vulkan", OPT_TYPE_BOOL, 0, {&no_vulkan}, "disable the Vulkan renderer"},
    {"gpu-params", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_gpu_params}, "backend configuration using a list of key=value pairs separated by ':'", "params"},
    {"vulkan-swap-mode", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_vulkan_swap_mode}, "present mode", "mode", swap_modes},
    {"max-glsl-version", OPT_TYPE_INT, 0, {&max_glsl_version}, "cap the GLSL version libplacebo targets (0 for no cap)", "version"},
    {"icc-profile", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_icc_profile}, "ICC profile passed to libplacebo", "path"},
    {"icc-auto", OPT_TYPE_BOOL, 0, {&icc_auto}, "use the ICC profile the display advertises"},
    {"no-display-hdr", OPT_TYPE_BOOL, 0, {&no_display_hdr}, "ignore the HDR peak brightness the display reports"},
    {"video-bg", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_video_bg}, "set the video background for transparent content (none, tiles, or a color)", "color"},
    {"hwaccel", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_hwaccel}, "use hardware accelerated decoding with the specified method, or no, or none, or off", "method"},
    {"no-hwaccel", OPT_TYPE_BOOL, 0, {&no_hwaccel}, "disable hardware accelerated decoding (force software)"},
    {"hwaccel-codecs", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_hwaccel_codecs}, "a list of codecs allowed to use hwaccel separated by ',', or all, with '-' before a name to exclude it (default all)", "codecs"},
    {"hwaccel-max-size", OPT_TYPE_INT, 0, {&hwaccel_max_size}, "the maximum size at which hwaccel is tried (0 to query the hardware or a negative for no limit)", "pixels"},
    {"max-texture-size", OPT_TYPE_INT, 0, {&max_texture_size}, "the maximum texture size (0 to query the hardware or a negative for no limit)", "pixels"},
    {"video-fill", OPT_TYPE_BOOL, 0, {&video_fill}, "scale video to fill the window"},
    {"360-sbs", OPT_TYPE_BOOL, 0, {&enable_360sbs}, "enable 360\xc2\xb0 equirectangular projection for side-by-side video"},
    {"360-tb", OPT_TYPE_BOOL, 0, {&enable_360tb}, "enable 360\xc2\xb0 equirectangular projection for top-bottom video"},
    {"360-eq", OPT_TYPE_BOOL, 0, {&enable_360eq}, "enable 360\xc2\xb0 spherical projection for side-by-side video"},
    {"360-eq-tb", OPT_TYPE_BOOL, 0, {&enable_360eqtb}, "enable 360\xc2\xb0 spherical projection for top-bottom video"},
    {"no-ytdl", OPT_TYPE_BOOL, 0, {&ytdl_disable}, "disable yt-dlp integration"},
    {"ytdl-path", OPT_TYPE_STRING, 0, {&ytdl_path}, "path to the yt-dlp binary", "path"},
    {"ytdl-format", OPT_TYPE_STRING, 0, {&ytdl_format}, "yt-dlp format selection string", "format"},
    {"delete", OPT_TYPE_BOOL, 0, {&allow_delete}, "enable permanent file deletion"},
    {"no-terminal-quit", OPT_TYPE_BOOL, 0, {&terminal_quit_disable}, "disable the terminal quit keybinding"},
    {"display-fps", OPT_TYPE_DOUBLE, 0, {&display_fps_override}, "override the detected display refresh rate", "fps"},
    {"no-vsync-snap", OPT_TYPE_BOOL, 0, {&no_vsync_snap}, "disable snapping frame deadlines to the display refresh grid"},
    {"interpolate", OPT_TYPE_BOOL, 0, {&frame_interpolation}, "oversample interpolation"},
    {"deinterlace", OPT_TYPE_BOOL, 0, {&deinterlace}, "deinterlace with YADIF"},
    {"supersample", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_ARG_OPTIONAL, {.func_arg = opt_supersample}, "sharpen and deband video", "level", supersample_level_names, "medium", "off"},
    {"scaler", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_scale}, "set the scaler (or help)", "filter"},
    {"r", OPT_TYPE_DOUBLE, 0, {&fps_convert}, "convert video to this frame rate with the fps filter", "fps"},
    {
        NULL,
    },
};
#pragma GCC diagnostic pop

/* clang-format off */
#define PRINT_LIB_VERSION(libname, LIBNAME)                                              \
    av_log(NULL, AV_LOG_INFO, "  lib%-11s %2d.%3d.%3d / %2d.%3d.%3d\n", #libname,        \
           LIB##LIBNAME##_VERSION_MAJOR, LIB##LIBNAME##_VERSION_MINOR,                   \
           LIB##LIBNAME##_VERSION_MICRO, AV_VERSION_MAJOR(libname##_version()),          \
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
    av_log(NULL, AV_LOG_INFO, "  lib%-11s %s\n", "placebo", PL_VERSION);

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
        error = "Expected number for %s but found %s.\n";
    } else if (isnan(d) || isinf(d)) {
        error = "Expected a finite number for %s but found %s.\n";
    } else if (d < min || d > max) {
        error = "The value for %s was %s which is not within %f - %f.\n";
    } else if (type == OPT_TYPE_INT64 && (int64_t)d != d) {
        error = "Expected int64 for %s but found %s.\n";
    } else if (type == OPT_TYPE_INT && (int)d != d) {
        error = "Expected int for %s but found %s.\n";
    } else {
        *dst = d;
        return 0;
    }
    log_dead(error, context, numstr, min, max);

    return AVERROR(EINVAL);
}

const char *option_name(const OptionDef *defs, const void *dst) {
    const OptionDef *po;

    for (po = defs; po->name; po++) {
        if (po->type != OPT_TYPE_FUNC && po->type != OPT_TYPE_ALIAS &&
            po->u.dst_ptr == dst) {
            break;
        }
    }
    av_assert0(po->name);

    return po->name;
}

static int format_option_name(const OptionDef *defs, const OptionDef *po,
                              char *buf, size_t size) {
    if (po->type == OPT_TYPE_ALIAS) {
        return 0;
    }
    av_strlcpy(buf, po->name, size);

    for (const OptionDef *alias = defs; alias->name; alias++) {
        if (alias->type == OPT_TYPE_ALIAS && !strcmp(alias->u.alias_of, po->name)) {
            av_strlcatf(buf, size, ", -%s", alias->name);
        }
    }

    if (po->argname && po->argname[0]) {
        if (po->flags & OPT_ARG_OPTIONAL) {
            av_strlcatf(buf, size, " [%s]", po->argname);
        } else {
            av_strlcatf(buf, size, " <%s>", po->argname);
        }
    }

    return strlen(buf) + 1;
}

static const char *format_option_values(const OptionDef *po, char *buf, size_t size) {
    char list[192];

    if (!po->values) {
        return "";
    }
    option_value_list(po->values, NULL, list, sizeof(list));
    if ((po->flags & OPT_ARG_OPTIONAL) && po->implied) {
        snprintf(buf, size, " (%s, or implied %s)", list, po->implied);
    } else {
        snprintf(buf, size, " (%s)", list);
    }

    return buf;
}

void show_help_options(const OptionDef *defs) {
    const OptionDef *po;
    char buf[128];
    char values[256];
    int max_width = 0;

    for (po = defs; po->name; po++) {
        int width = format_option_name(defs, po, buf, sizeof(buf));
        if (width > max_width) {
            max_width = width;
        }
    }

    for (po = defs; po->name; po++) {
        if (format_option_name(defs, po, buf, sizeof(buf))) {
            printf("-%-*s  %s%s\n", max_width, buf, po->help,
                   format_option_values(po, values, sizeof(values)));
        }
    }
}

int opt_help(void *optctx av_unused, const char *opt av_unused,
             const char *arg av_unused) {
    show_help_default();
    return 0;
}

static const OptionDef *find_option_entry(const OptionDef *po, const char *name) {
    while (po->name) {
        const char *end;
        if (av_strstart(name, po->name, &end) && (!*end || *end == ':')) {
            break;
        }
        po++;
    }
    return po;
}

static const OptionDef *find_option(const OptionDef *defs, const char *name) {
    const OptionDef *po = find_option_entry(defs, name);

    if (po->type == OPT_TYPE_ALIAS) {
        po = find_option_entry(defs, po->u.alias_of);
        av_assert0(po->name && po->type != OPT_TYPE_ALIAS);
    }

    return po;
}

enum OptionOrigin {
    OPT_FROM_NOWHERE = 0,
    OPT_FROM_CONFIG,
    OPT_FROM_CMDLINE,
};

static uint8_t option_origin[FF_ARRAY_ELEMS(options)];
static const OptionDef *option_given_as[FF_ARRAY_ELEMS(options)];

static void note_option_origin(const OptionDef *defs, const OptionDef *po,
                               enum OptionOrigin origin, const char *opt,
                               const char *arg) {
    size_t i = (size_t)(po - defs);

    if (po->type == OPT_TYPE_BOOL) {
        if (!*(int *)po->u.dst_ptr) {
            origin = OPT_FROM_NOWHERE;
        }
    } else if (!arg || !arg[0] ||
               (po->implied_no && !strcmp(arg, po->implied_no))) {
        origin = OPT_FROM_NOWHERE;
    }
    if (i < FF_ARRAY_ELEMS(option_origin)) {
        const OptionDef *given = find_option_entry(defs, opt);

        option_origin[i] = origin;
        option_given_as[i] = given->name ? given : po;
    }
}

const char *option_first_from_cmdline(const OptionDef *defs, const char *except) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(option_origin); i++) {
        if (option_origin[i] != OPT_FROM_CMDLINE || !defs[i].name) {
            continue;
        }
        if (except && !strcmp(defs[i].name, except)) {
            continue;
        }

        return option_given_as[i] ? option_given_as[i]->name : defs[i].name;
    }

    return NULL;
}

int option_given_on_cmdline(const OptionDef *defs, const char *name) {
    const OptionDef *po = find_option(defs, name);
    size_t i = (size_t)(po - defs);

    if (!po->name || i >= FF_ARRAY_ELEMS(option_origin)) {
        return 0;
    }

    return option_origin[i] == OPT_FROM_CMDLINE;
}

static const char *option_name_given(const OptionDef *defs, const char *name) {
    const OptionDef *po = find_option(defs, name);
    size_t i = (size_t)(po - defs);

    if (!po->name || i >= FF_ARRAY_ELEMS(option_given_as) || !option_given_as[i]) {
        return name;
    }

    return option_given_as[i]->name;
}

static enum OptionOrigin option_origin_of(const OptionDef *defs, const char *name) {
    const OptionDef *po = find_option(defs, name);
    size_t i = (size_t)(po - defs);

    if (!po->name || i >= FF_ARRAY_ELEMS(option_origin)) {
        return OPT_FROM_NOWHERE;
    }

    return option_origin[i];
}

enum OptionRelation {
    OPT_CONFLICTS_WITH,
    OPT_DISABLES,
    OPT_IMPLIES,
};

static const struct {
    const char *a;
    const char *b;
    enum OptionRelation how;
} option_relations[] = {
    {"360-sbs", "360-tb", OPT_CONFLICTS_WITH},
    {"360-sbs", "360-eq", OPT_CONFLICTS_WITH},
    {"360-sbs", "360-eq-tb", OPT_CONFLICTS_WITH},
    {"360-tb", "360-eq", OPT_CONFLICTS_WITH},
    {"360-tb", "360-eq-tb", OPT_CONFLICTS_WITH},
    {"360-eq", "360-eq-tb", OPT_CONFLICTS_WITH},

    {"nodisp", "vn", OPT_IMPLIES},
    {"nodisp", "single", OPT_DISABLES},
    {"benchmark", "an", OPT_IMPLIES},

    {"vn", "vf", OPT_DISABLES},
    {"vn", "r", OPT_DISABLES},
    {"vn", "rotate", OPT_DISABLES},
    {"vn", "no-autorotate", OPT_DISABLES},
    {"vn", "deinterlace", OPT_DISABLES},
    {"vn", "interpolate", OPT_DISABLES},
    {"vn", "supersample", OPT_DISABLES},
    {"vn", "scaler", OPT_DISABLES},
    {"vn", "video-bg", OPT_DISABLES},
    {"vn", "video-fill", OPT_DISABLES},
    {"vn", "vcodec", OPT_DISABLES},
    {"vn", "slow", OPT_DISABLES},
    {"vn", "max-texture-size", OPT_DISABLES},
    {"vn", "hwaccel", OPT_DISABLES},
    {"vn", "hwaccel-codecs", OPT_DISABLES},
    {"vn", "hwaccel-max-size", OPT_DISABLES},
    {"vn", "360-sbs", OPT_DISABLES},
    {"vn", "360-tb", OPT_DISABLES},
    {"vn", "360-eq", OPT_DISABLES},
    {"vn", "360-eq-tb", OPT_DISABLES},

    {"an", "af", OPT_DISABLES},
    {"an", "volume", OPT_DISABLES},
    {"an", "normalize", OPT_DISABLES},
    {"an", "normalize-target", OPT_DISABLES},
    {"an", "normalize-gain", OPT_DISABLES},
    {"an", "mute", OPT_DISABLES},
    {"an", "acodec", OPT_DISABLES},
    {"an", "audio-spdif", OPT_DISABLES},
    {"an", "audio-spdif-force", OPT_DISABLES},

    {"sn", "scodec", OPT_DISABLES},
    {"sn", "sub-offset", OPT_DISABLES},

    {"nodisp", "windowed", OPT_DISABLES},
    {"nodisp", "autofit", OPT_DISABLES},
    {"nodisp", "alwaysontop", OPT_DISABLES},
    {"nodisp", "window-title", OPT_DISABLES},
    {"nodisp", "delete", OPT_DISABLES},
    {"nodisp", "icc-profile", OPT_DISABLES},
    {"nodisp", "icc-auto", OPT_DISABLES},
    {"nodisp", "no-display-hdr", OPT_DISABLES},
    {"nodisp", "gpu-api", OPT_DISABLES},
    {"nodisp", "gpu-params", OPT_DISABLES},
    {"nodisp", "vulkan-swap-mode", OPT_DISABLES},
    {"nodisp", "max-glsl-version", OPT_DISABLES},
    {"nodisp", "display-fps", OPT_DISABLES},
    {"nodisp", "no-vsync-snap", OPT_DISABLES},
    {"nodisp", "shader-cache-dir", OPT_DISABLES},

    {"no-shader-cache", "shader-cache-dir", OPT_DISABLES},
    {"no-ytdl", "ytdl-path", OPT_DISABLES},
    {"no-ytdl", "ytdl-format", OPT_DISABLES},
    {"icc-profile", "icc-auto", OPT_DISABLES},
};

static enum OptionOrigin option_reach(const OptionDef *defs, const char *name,
                                      const char **via) {
    enum OptionOrigin origin = option_origin_of(defs, name);

    *via = option_name_given(defs, name);
    if (origin) {
        return origin;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(option_relations); i++) {
        if (option_relations[i].how != OPT_IMPLIES ||
            strcmp(option_relations[i].b, name)) {
            continue;
        }
        origin = option_origin_of(defs, option_relations[i].a);
        if (origin) {
            *via = option_name_given(defs, option_relations[i].a);
            return origin;
        }
    }

    return OPT_FROM_NOWHERE;
}

static void option_forget(const OptionDef *defs, const char *name) {
    const OptionDef *po = find_option(defs, name);
    size_t i = (size_t)(po - defs);

    if (!po->name) {
        return;
    }
    av_assert0(po->type == OPT_TYPE_BOOL || po->type == OPT_TYPE_STRING);
    if (po->type == OPT_TYPE_BOOL) {
        *(int *)po->u.dst_ptr = 0;
    } else {
        av_freep(po->u.dst_ptr);
    }
    if (i < FF_ARRAY_ELEMS(option_origin)) {
        option_origin[i] = OPT_FROM_NOWHERE;
        option_given_as[i] = NULL;
    }
}

void validate_option_tables(const OptionDef *defs) {
    for (const OptionDef *po = defs; po->name; po++) {
        if (po->type != OPT_TYPE_ALIAS) {
            continue;
        }
        const OptionDef *target = find_option_entry(defs, po->u.alias_of);
        av_assert0(target->name && target->type != OPT_TYPE_ALIAS);
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(option_relations); i++) {
        av_assert0(find_option(defs, option_relations[i].a)->name);
        av_assert0(find_option(defs, option_relations[i].b)->name);
    }
}

void validate_option_relations(const OptionDef *defs) {
    for (size_t i = 0; i < FF_ARRAY_ELEMS(option_relations); i++) {
        const char *a = option_relations[i].a;
        const char *b = option_relations[i].b;
        const char *via;
        const char *given;
        enum OptionOrigin oa, ob;

        if (option_relations[i].how == OPT_IMPLIES) {
            continue;
        }
        oa = option_reach(defs, a, &via);
        ob = option_origin_of(defs, b);
        if (!oa || !ob) {
            continue;
        }
        given = option_name_given(defs, b);
        if (option_relations[i].how == OPT_DISABLES) {
            if (ob == OPT_FROM_CMDLINE) {
                log_warn("-%s does nothing because -%s was given.\n", given,
                         via);
            }
            continue;
        }
        if (oa == ob) {
            fatal_quit("-%s and -%s are mutually exclusive.\n", via, given);
        }
        if (oa == OPT_FROM_CONFIG) {
            log_warn("-%s from the configuration file is ignored because -%s "
                     "was given.\n",
                     via, given);
            option_forget(defs, via);
        } else {
            log_warn("-%s from the configuration file is ignored because -%s "
                     "was given.\n",
                     given, via);
            option_forget(defs, b);
        }
    }
}

#define OPTION_NAME_MAX 128

static const char *split_option_name(const char *opt, char *buf, size_t size) {
    const char *eq = strchr(opt, '=');
    if (!eq) {
        return NULL;
    }

    size_t len = eq - opt;
    if (len >= size) {
        len = size - 1;
    }
    av_strlcpy(buf, opt, len + 1);

    return eq + 1;
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

static int opt_is_value(const OptionDef *po, const char *arg) {
    if (po->is_value) {
        return po->is_value(arg);
    }

    return po->values && opt_value_index(arg, po->values) >= 0;
}

static const char *opt_implied_value(const OptionDef *po, const char *arg) {
    if (!(po->flags & OPT_ARG_OPTIONAL) || opt_is_value(po, arg)) {
        return arg;
    }

    int on = config_parse_bool(arg);

    return on < 0 ? arg : (on ? po->implied : po->implied_no);
}

static int opt_wants_next(const OptionDef *po, const char *next) {
    if (!opt_has_arg(po) || !next) {
        return 0;
    }
    if (!(po->flags & OPT_ARG_OPTIONAL)) {
        return 1;
    }

    return config_parse_bool(next) >= 0 || opt_is_value(po, next);
}

static int opt_negatable(const OptionDef *po) {
    if (!po->name) {
        return 0;
    }

    return po->type == OPT_TYPE_BOOL || (po->flags & OPT_ARG_OPTIONAL);
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
            log_dead("Invalid duration for option -%s: %s.\n", opt, arg);
            return ret;
        }
    } else if (po->type == OPT_TYPE_FLOAT) {
        ret = parse_number(opt, arg, OPT_TYPE_FLOAT, -LACHESIS_INF, LACHESIS_INF, &num);
        if (ret < 0) {
            return ret;
        }
        *(float *)dst = num;
    } else if (po->type == OPT_TYPE_DOUBLE) {
        ret = parse_number(opt, arg, OPT_TYPE_DOUBLE, -LACHESIS_INF, LACHESIS_INF, &num);
        if (ret < 0) {
            return ret;
        }
        *(double *)dst = num;
    } else {
        av_assert0(po->type == OPT_TYPE_FUNC && po->u.func_arg);
        ret = po->u.func_arg(optctx, opt, arg);
        if (ret < 0) {
            if (ret != AVERROR(EINVAL) && ret != AVERROR_EXIT) {
                log_dead("Failed to set value '%s' for option '%s': %s.\n",
                         arg, opt, av_err2str(ret));
            }
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
    char name[OPTION_NAME_MAX];
    const char *inline_arg = split_option_name(opt, name, sizeof(name));
    const OptionDef *po;
    int negated = 0;
    int consumed;
    int ret;

    if (inline_arg) {
        opt = name;
    }

    po = find_option(defs, opt);
    if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
        const OptionDef *neg = find_option(defs, opt + 2);
        if (opt_negatable(neg)) {
            po = neg;
            negated = 1;
        }
    }

    if (!po->name) {
        log_dead("Unrecognized option '%s'.\n", opt);
        return AVERROR(EINVAL);
    }

    consumed = !inline_arg && !negated && opt_wants_next(po, arg);

    if (!opt_has_arg(po) || negated) {
        int on = 1;

        if (inline_arg) {
            on = config_parse_bool(inline_arg);
            if (on < 0) {
                log_dead("Option '%s' wants yes or no, got '%s'.\n", opt, inline_arg);
                return AVERROR(EINVAL);
            }
        }
        if (negated) {
            on = !on;
        }
        if (opt_has_arg(po)) {
            arg = on ? po->implied : po->implied_no;
        } else {
            if (po->type != OPT_TYPE_BOOL && !on) {
                return 0;
            }
            arg = on ? "1" : "0";
        }
    } else if (inline_arg || consumed) {
        arg = opt_implied_value(po, inline_arg ? inline_arg : arg);
    } else {
        if (!(po->flags & OPT_ARG_OPTIONAL)) {
            log_dead("Missing argument for option '%s'.\n", opt);
            return AVERROR(EINVAL);
        }
        if ((po->flags & OPT_STRICT_VALUE) && arg && arg[0] &&
            !(arg[0] == '-' && arg[1]) && !playlist_path_is_usable(arg)) {
            log_dead("'%s' is neither an input nor a value for -%s: %s.\n",
                     arg, opt, po->help);
            return AVERROR(EINVAL);
        }
        arg = po->implied;
    }

    ret = write_option(optctx, po, opt, arg);
    if (ret < 0) {
        return ret;
    }
    note_option_origin(defs, po, OPT_FROM_CMDLINE, opt, arg);

    return consumed;
}

int parse_config_option(void *optctx, const char *opt, const char *arg,
                        const OptionDef *defs, const char *src) {
    const OptionDef *po = find_option(defs, opt);
    int ret;

    if (!po->name) {
        log_dead("%s: unknown option '%s'.\n", src, opt);
        return AVERROR(EINVAL);
    }
    /* For example --help, --version... */
    if (po->flags & OPT_EXIT) {
        log_dead("%s: option '%s' is not allowed here.\n", src, opt);
        return AVERROR(EINVAL);
    }
    if (po->flags & OPT_CMDLINE_ONLY) {
        log_dead("%s: option '%s' is only accepted on the command line.\n",
                 src, opt);
        return AVERROR(EINVAL);
    }

    if (po->flags & OPT_ARG_OPTIONAL) {
        arg = arg && arg[0] ? opt_implied_value(po, arg) : po->implied;
    }

    if (po->type == OPT_TYPE_BOOL || !opt_has_arg(po)) {
        int on = config_parse_bool(arg);
        if (on < 0) {
            log_dead("%s: option '%s' wants yes or no, got '%s'.\n",
                     src, opt, arg);
            return AVERROR(EINVAL);
        }
        if (po->type != OPT_TYPE_BOOL && !on) {
            return 0;
        }
        arg = on ? "1" : "0";
    } else if (!arg || (!arg[0] && po->type != OPT_TYPE_STRING && po->type != OPT_TYPE_FUNC)) {
        log_dead("%s: option '%s' needs a value.\n", src, opt);
        return AVERROR(EINVAL);
    }

    ret = write_option(optctx, po, opt, arg);
    if (ret < 0) {
        if (ret != AVERROR_EXIT) {
            log_dead("%s: option '%s' was not applied.\n", src, opt);
        }
        return ret;
    }
    note_option_origin(defs, po, OPT_FROM_CONFIG, opt, arg);

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
                         const char *optname, const char **value_out) {
    const OptionDef *po;
    int negated;
    int i;

    *value_out = NULL;

    for (i = 1; i < argc; i++) {
        char name[OPTION_NAME_MAX];
        const char *cur_opt = argv[i];
        const char *inline_arg;

        if (!(cur_opt[0] == '-' && cur_opt[1])) {
            continue;
        }
        cur_opt++;

        if (cur_opt[0] == '-' && cur_opt[1] != '\0') {
            cur_opt++;
        }

        inline_arg = split_option_name(cur_opt, name, sizeof(name));
        if (inline_arg) {
            cur_opt = name;
        }

        negated = 0;
        po = find_option(defs, cur_opt);
        if (!po->name && cur_opt[0] == 'n' && cur_opt[1] == 'o') {
            const OptionDef *neg = find_option(defs, cur_opt + 2);
            if (opt_negatable(neg)) {
                po = neg;
                negated = 1;
            }
        }

        if ((!po->name && !strcmp(cur_opt, optname)) ||
            (po->name && !strcmp(optname, po->name))) {
            *value_out = inline_arg;
            return i;
        }

        if (!inline_arg &&
            (!po->name ||
             (!negated &&
              opt_wants_next(po, i + 1 < argc ? argv[i + 1] : NULL)))) {
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
            log_dead("Invalid loglevel \"%s\". Possible levels are numbers or:\n", arg);
            for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
                log_dead("\"%s\"\n", log_levels[i].name);
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
    const char *value;
    int idx = locate_option(argc, argv, defs, "loglevel", &value);
    if (!idx) {
        return;
    }
    if (!value) {
        value = argv[idx + 1];
    }
    if (value) {
        opt_loglevel(NULL, "loglevel", value);
    }
}

int opt_quiet(void *optctx av_unused, const char *opt av_unused,
              const char *arg av_unused) {
    lachesis_quiet = 1;
    av_log_set_level(AV_LOG_QUIET);

    return 0;
}

void parse_quiet(int argc, char **argv, const OptionDef *defs) {
    const char *value;
    if (!locate_option(argc, argv, defs, "quiet", &value)) {
        return;
    }
    if (value && config_parse_bool(value) == 0) {
        return;
    }
    opt_quiet(NULL, "quiet", NULL);
}

void parse_allow_unsafe(int argc, char **argv, const OptionDef *defs) {
    const char *value;
    int idx = locate_option(argc, argv, defs, "allow-unsafe", &value);
    if (!idx) {
        return;
    }
    const char *name = argv[idx];
    while (*name == '-') {
        name++;
    }
    if (!strncmp(name, "no", 2)) {
        return;
    }
    if (value && config_parse_bool(value) == 0) {
        return;
    }
    allow_unsafe = 1;
}

void parse_all_files(int argc, char **argv, const OptionDef *defs) {
    const char *value;
    int idx = locate_option(argc, argv, defs, "all-files", &value);
    if (!idx) {
        return;
    }
    const char *name = argv[idx];
    while (*name == '-') {
        name++;
    }
    if (!strncmp(name, "no", 2)) {
        return;
    }
    if (value && config_parse_bool(value) == 0) {
        return;
    }
    all_files = 1;
}
