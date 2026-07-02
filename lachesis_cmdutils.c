/*
 * An FFmpeg stub, which is:
 *   Copyright © 2000-2003 Fabrice Bellard, and the FFmpeg authors.
 * License: GNU LGPL v2.1 or later, as the originals.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/eval.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>

#include "lachesis_cmdutils.h"

#ifdef _WIN32
#include <windows.h>
#endif

AVDictionary *sws_dict;
AVDictionary *swr_opts;
AVDictionary *format_opts, *codec_opts;

void uninit_opts(void) {
    av_dict_free(&swr_opts);
    av_dict_free(&sws_dict);
    av_dict_free(&format_opts);
    av_dict_free(&codec_opts);
}

void init_dynload(void) {
#ifdef _WIN32
    /* Remove the current working directory from the DLL search path as a
     * security precaution. */
    SetDllDirectoryW(L"");
#endif
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

void show_help_options(const OptionDef *options, int req_flags,
                       int rej_flags) {
    const OptionDef *po;
    int first;
    int max_width = 0;

    for (po = options; po->name; po++) {
        int width = strlen(po->name) + 1; /* +1 for the leading '-' */
        if (po->argname) {
            width += strlen(po->argname) + 3; /* +3 for " <>" */
        }
        if (width > max_width) {
            max_width = width;
        }
    }

    first = 1;
    for (po = options; po->name; po++) {
        char buf[128];

        if (((po->flags & req_flags) != req_flags) || (po->flags & rej_flags)) {
            continue;
        }

        if (first) {
            first = 0;
        }
        av_strlcpy(buf, po->name, sizeof(buf));

        if (po->argname) {
            av_strlcatf(buf, sizeof(buf), " <%s>", po->argname);
        }

        printf("-%-*s  %s\n", max_width, buf, po->help);
    }
}

int opt_help(void *optctx av_unused, const char *opt av_unused, const char *arg) {
    show_help_default(arg, NULL);
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
                        const char *arg, const OptionDef *defs av_unused) {
    /* lachesis's options only use global destination pointers; the SPEC /
     * per-stream / offset and "/file" branches of the original are omitted. */
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
                 const OptionDef *options) {
    const OptionDef *po;
    int ret;

    po = find_option(options, opt);
    if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
        /* handle 'no' bool option */
        po = find_option(options, opt + 2);
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

    ret = write_option(optctx, po, opt, arg, options);
    if (ret < 0) {
        return ret;
    }

    return opt_has_arg(po);
}

int parse_options(void *optctx, int argc, char **argv, const OptionDef *options,
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

            if ((ret = parse_option(optctx, opt, argv[optindex], options)) < 0) {
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

static int locate_option(int argc, char **argv, const OptionDef *options,
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

        po = find_option(options, cur_opt);
        if (!po->name && cur_opt[0] == 'n' && cur_opt[1] == 'o') {
            po = find_option(options, cur_opt + 2);
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
    av_log_set_level(level);
    return 0;
}

void parse_loglevel(int argc, char **argv, const OptionDef *options) {
    int idx = locate_option(argc, argv, options, "loglevel");
    if (idx && argv[idx + 1]) {
        opt_loglevel(NULL, "loglevel", argv[idx + 1]);
    }
}

int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec) {
    int ret = avformat_match_stream_specifier(s, st, spec);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    }
    return ret;
}

int filter_codec_opts(const AVDictionary *opts, enum AVCodecID codec_id av_unused,
                      AVFormatContext *s, AVStream *st, const AVCodec *codec,
                      AVDictionary **dst, AVDictionary **opts_used) {
    AVDictionary *ret = NULL;
    const AVDictionaryEntry *t = NULL;
    int flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                           : AV_OPT_FLAG_DECODING_PARAM;
    char prefix = 0;
    const AVClass *cc = avcodec_get_class();

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix = 'v';
        flags |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix = 'a';
        flags |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix = 's';
        flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    default:
        break;
    }

    while ((t = av_dict_iterate(opts, t))) {
        const AVClass *priv_class;
        char *p = strchr(t->key, ':');
        int used = 0;

        if (p) {
            int err = check_stream_specifier(s, st, p + 1);
            if (err < 0) {
                av_dict_free(&ret);
                return err;
            } else if (!err) {
                continue;
            }
            *p = 0;
        }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            ((priv_class = codec->priv_class) &&
             av_opt_find(&priv_class, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ))) {
            av_dict_set(&ret, t->key, t->value, 0);
            used = 1;
        } else if (t->key[0] == prefix &&
                   av_opt_find(&cc, t->key + 1, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&ret, t->key + 1, t->value, 0);
            used = 1;
        }

        if (p) {
            *p = ':';
        }

        if (used && opts_used) {
            av_dict_set(opts_used, t->key, "", 0);
        }
    }

    *dst = ret;
    return 0;
}

int setup_find_stream_info_opts(AVFormatContext *s,
                                AVDictionary *local_codec_opts,
                                AVDictionary ***dst) {
    int ret;
    AVDictionary **opts;

    *dst = NULL;

    if (!s->nb_streams) {
        return 0;
    }

    opts = av_calloc(s->nb_streams, sizeof(*opts));
    if (!opts) {
        return AVERROR(ENOMEM);
    }

    for (unsigned int i = 0; i < s->nb_streams; i++) {
        ret = filter_codec_opts(local_codec_opts, s->streams[i]->codecpar->codec_id,
                                s, s->streams[i], NULL, &opts[i], NULL);
        if (ret < 0) {
            goto fail;
        }
    }
    *dst = opts;
    return 0;
fail:
    for (unsigned int i = 0; i < s->nb_streams; i++) {
        av_dict_free(&opts[i]);
    }
    av_freep(&opts);
    return ret;
}

int check_avoptions(AVDictionary *m) {
    const AVDictionaryEntry *t = av_dict_iterate(m, NULL);
    if (t) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }
    return 0;
}

void remove_avoptions(AVDictionary **a, AVDictionary *b) {
    const AVDictionaryEntry *t = NULL;

    while ((t = av_dict_iterate(b, t))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

double get_rotation(const int32_t *displaymatrix) {
    double theta = 0;
    if (displaymatrix) {
        theta = -round(av_display_rotation_get(displaymatrix));
    }

    theta -= 360 * floor(theta / 360 + 0.9 / 360);

    if (fabs(theta - 90 * round(theta / 90)) > 2) {
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n");
    }

    return theta;
}

int grow_array(void **array, int elem_size, int *size, int new_size) {
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
