/*
 * An FFmpeg stub, which is:
 * Copyright © 2000-2003 Fabrice Bellard, and the FFmpeg authors.
 */

#ifndef LACHESIS_CMDUTILS_H
#define LACHESIS_CMDUTILS_H

#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#undef main /* We don't want SDL to override our main(). */
#endif

extern const char program_name[];
extern const int program_birth_year;

extern AVDictionary *sws_dict;
extern AVDictionary *swr_opts;
extern AVDictionary *format_opts, *codec_opts;

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
#define OPT_EXPERT (1 << 2)
#define OPT_VIDEO (1 << 3)
#define OPT_AUDIO (1 << 4)
#define OPT_SUBTITLE (1 << 5)
#define OPT_DATA (1 << 6)
#define OPT_PERFILE (1 << 7)
#define OPT_FLAG_OFFSET (1 << 8)
#define OPT_OFFSET (OPT_FLAG_OFFSET | OPT_PERFILE)
#define OPT_FLAG_SPEC (1 << 9)
#define OPT_SPEC (OPT_FLAG_SPEC | OPT_OFFSET)
#define OPT_FLAG_PERSTREAM (1 << 10)
#define OPT_PERSTREAM (OPT_FLAG_PERSTREAM | OPT_SPEC)
#define OPT_INPUT (1 << 11)
#define OPT_OUTPUT (1 << 12)
#define OPT_HAS_ALT (1 << 13)
#define OPT_HAS_CANON (1 << 14)
#define OPT_DECODER (1 << 15)

typedef struct OptionDef {
    const char *name;
    enum OptionType type;
    int flags;

    union {
        void *dst_ptr;
        int (*func_arg)(void *, const char *, const char *);
        size_t off;
    } u;
    const char *help;
    const char *argname;

    union {
        const char *name_canon;
        const char *const *names_alt;
    } u1;
} OptionDef;

void init_dynload(void);
void uninit_opts(void);

int parse_number(const char *context, const char *numstr, enum OptionType type,
                 double min, double max, double *dst);

void show_help_options(const OptionDef *options, int req_flags, int rej_flags);
void show_help_default(const char *opt, const char *arg);

int parse_options(void *optctx, int argc, char **argv, const OptionDef *options,
                  int (*parse_arg_function)(void *optctx, const char *));
int parse_option(void *optctx, const char *opt, const char *arg,
                 const OptionDef *options);
void parse_loglevel(int argc, char **argv, const OptionDef *options);

int opt_loglevel(void *optctx, const char *opt, const char *arg);

int filter_codec_opts(const AVDictionary *opts, enum AVCodecID codec_id,
                      AVFormatContext *s, AVStream *st, const AVCodec *codec,
                      AVDictionary **dst, AVDictionary **opts_used);
int setup_find_stream_info_opts(AVFormatContext *s,
                                AVDictionary *local_codec_opts,
                                AVDictionary ***dst);
int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec);

int check_avoptions(AVDictionary *m);
void remove_avoptions(AVDictionary **a, AVDictionary *b);
double get_rotation(const int32_t *displaymatrix);

int grow_array(void **array, int elem_size, int *size, int new_size);
#define GROW_ARRAY(array, nb_elems) \
    grow_array((void **)&array, sizeof(*array), &nb_elems, nb_elems + 1)

static inline void print_error(const char *filename, int err) {
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, av_err2str(err));
}

/* clang-format off */
#define CMDUTILS_COMMON_OPTIONS \
    {"h", OPT_TYPE_FUNC, OPT_EXIT, {.func_arg = opt_help}, "show help"},                                                 \
    {"?", OPT_TYPE_FUNC, OPT_EXIT | OPT_EXPERT, {.func_arg = opt_help}, "show help"},                                    \
    {"help", OPT_TYPE_FUNC, OPT_EXIT | OPT_EXPERT, {.func_arg = opt_help}, "show help"},                                 \
    {"-help", OPT_TYPE_FUNC, OPT_EXIT | OPT_EXPERT, {.func_arg = opt_help}, "show help"},                                \
    {"loglevel", OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, {.func_arg = opt_loglevel}, "set logging level", "loglevel"}, \
    {"v", OPT_TYPE_FUNC, OPT_FUNC_ARG, {.func_arg = opt_loglevel}, "set logging level", "loglevel"},
/* clang-format on */
int opt_help(void *optctx, const char *opt, const char *arg);

#endif /* LACHESIS_CMDUTILS_H */
