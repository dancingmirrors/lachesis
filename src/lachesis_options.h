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

#ifndef LACHESIS_OPTIONS_H
#define LACHESIS_OPTIONS_H

#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "lachesis_internal.h"

extern const char program_name[];
extern const int program_birth_year;

enum OptionType {
    OPT_TYPE_FUNC,
    OPT_TYPE_BOOL,
    OPT_TYPE_STRING,
    OPT_TYPE_INT,
    OPT_TYPE_INT64,
    OPT_TYPE_FLOAT,
    OPT_TYPE_DOUBLE,
    OPT_TYPE_TIME,
};

#define OPT_FUNC_ARG (1 << 0)
#define OPT_EXIT (1 << 1)

typedef struct OptionDef {
    const char *name;
    enum OptionType type;
    int flags;

    union {
        void *dst_ptr;
        int (*func_arg)(void *, const char *, const char *);
    } u;
    const char *help;
    const char *argname;
} OptionDef;

int parse_number(const char *context, const char *numstr, enum OptionType type,
                 double min, double max, double *dst);

void show_help_options(const OptionDef *defs);
void show_help_default(void);

int parse_options(void *optctx, int argc, char **argv, const OptionDef *defs,
                  int (*parse_arg_function)(void *optctx, const char *));
int parse_option(void *optctx, const char *opt, const char *arg,
                 const OptionDef *defs);
int parse_config_option(void *optctx, const char *opt, const char *arg,
                        const OptionDef *defs, const char *src);
void parse_loglevel(int argc, char **argv, const OptionDef *defs);
void parse_quiet(int argc, char **argv, const OptionDef *defs);

int opt_loglevel(void *optctx, const char *opt, const char *arg);
int opt_quiet(void *optctx, const char *opt, const char *arg);

/* clang-format off */
#define CMDUTILS_COMMON_OPTIONS \
    {"h", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_help}, "show help"},                  \
    {"?", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_help}, "show help"},     \
    {"help", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_help}, "show help"},  \
    {"-help", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_help}, "show help"}, \
    {"loglevel", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_loglevel}, "set FFmpeg's logging level", "loglevel"},
/* clang-format on */
int opt_help(void *optctx, const char *opt, const char *arg);

extern const OptionDef options[];

int opt_version(void *optctx, const char *opt, const char *arg);

extern const AVInputFormat *file_iformat;
extern const char *window_title;
extern int cmd_width;
extern int cmd_height;
extern int screen_left;
extern int screen_top;
extern int audio_disable;
extern int video_disable;
extern int subtitle_disable;
extern const char *wanted_stream_spec[AVMEDIA_TYPE_NB];
extern int seek_by_bytes;
extern float seek_interval;
extern int display_disable;
extern int benchmark;
extern int borderless;
extern int alwaysontop;
extern int startup_volume;
extern int av_sync_type;
extern int av_sync_type_explicit;
extern int skip_to_keyframe;
extern int64_t start_time;
extern int64_t play_duration;
extern int keep_open;
extern int shuffle;
extern int reverse_playlist;
extern int start_paused;
extern int loop;
extern float opt_cache_secs;
extern int opt_cache_size_mb;
extern const char *audio_codec_name;
extern const char *subtitle_codec_name;
extern const char *video_codec_name;
extern const char **vfilters_list;
extern int nb_vfilters;
extern char *afilters_opt;
extern int autorotate;
extern int disable_autorotate;
extern int video_rotate;
extern int enable_vulkan;
extern int disable_vulkan;
extern char *vulkan_params;
extern char *vulkan_swap_mode;
extern int no_shader_cache;
extern char *shader_cache_dir;
extern const char *icc_profile;
extern char *video_background;
extern const char *hwaccel;
extern int no_hwaccel;
extern int video_unscaled;
extern int enable_360sbs;
extern int enable_360tb;
extern int is_fullscreen;
extern int start_windowed;
extern float autofit_larger;
extern int global_muted;
extern int ytdl_disable;
extern const char *ytdl_path;
extern const char *ytdl_format;
extern int allow_delete;
extern int terminal_quit_disable;
extern int allow_volume_boost;

#endif /* LACHESIS_OPTIONS_H */
