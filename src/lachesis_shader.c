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

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lachesis_alloc.h"
#include "lachesis_config.h"
#include "lachesis_log.h"
#include "lachesis_shader.h"

#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/eval.h>
#include <libavutil/macros.h>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/shaders/custom.h>

#define SHADER_MAX_BYTES (64 << 20)

int shader_enabled = 1;

static char *paths[SHADER_MAX];
static int num_paths;
static int num_config;
static AVDictionary *params;

static void drop_first(int num) {
    for (int i = 0; i < num; i++) {
        av_freep(&paths[i]);
    }
    memmove(paths, paths + num, (num_paths - num) * sizeof(*paths));
    num_paths -= num;
}

int shader_add(const char *path) {
    char *copy;

    if (num_config) {
        drop_first(num_config);
        num_config = 0;
    }
    if (num_paths >= SHADER_MAX) {
        return AVERROR(E2BIG);
    }
    copy = av_strdup(path);
    if (!copy) {
        return AVERROR(ENOMEM);
    }
    paths[num_paths++] = copy;

    return 0;
}

void shader_clear(void) {
    drop_first(num_paths);
    num_config = 0;
}

void shader_config_done(void) {
    num_config = num_paths;
}

int shader_count(void) {
    return num_paths;
}

const char *shader_path(int i) {
    return paths[i];
}

const char *shader_name(int i) {
    return av_basename(paths[i]);
}

void shader_free_all(void) {
    shader_clear();
    av_dict_free(&params);
}

int shader_params_set(const char *spec) {
    AVDictionary *dict = NULL;
    const AVDictionaryEntry *entry = NULL;
    int ret = av_dict_parse_string(&dict, spec, "=", ":", AV_DICT_MATCH_CASE);

    while (ret >= 0 && (entry = av_dict_iterate(dict, entry))) {
        if (!entry->key[0] || !entry->value[0]) {
            ret = AVERROR(EINVAL);
        }
    }
    if (ret < 0) {
        av_dict_free(&dict);
        return ret;
    }
    av_dict_free(&params);
    params = dict;

    return 0;
}

static int read_file(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    int ret = 0;

    if (!f) {
        return AVERROR(errno);
    }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET)) {
        ret = AVERROR(errno);
        goto done;
    }
    if (size > SHADER_MAX_BYTES) {
        ret = AVERROR(EFBIG);
        goto done;
    }
    buf = av_malloc((size_t)size + 1);
    if (!buf) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    *len = fread(buf, 1, (size_t)size, f);
    if (ferror(f)) {
        av_free(buf);
        ret = AVERROR(EIO);
        goto done;
    }
    buf[*len] = '\0';
    *out = buf;

done:
    fclose(f);

    return ret;
}

static const char *shader_stem(int i, char *buf, size_t size) {
    const char *dot;

    av_strlcpy(buf, shader_name(i), size);
    dot = strrchr(buf, '.');
    if (dot && dot != buf) {
        buf[dot - buf] = '\0';
    }

    return buf;
}

static const char *describe_range(const struct pl_hook_par *par, char *buf,
                                  size_t size) {
    switch (par->type) {
    case PL_VAR_SINT:
        snprintf(buf, size, "%d and %d", par->minimum.i, par->maximum.i);
        break;
    case PL_VAR_UINT:
        snprintf(buf, size, "%u and %u", par->minimum.u, par->maximum.u);
        break;
    default:
        snprintf(buf, size, "%g and %g", (double)par->minimum.f,
                 (double)par->maximum.f);
        break;
    }

    return buf;
}

static int parse_value(const struct pl_hook_par *par, const char *value,
                       pl_var_data *out) {
    char *tail;

    if (par->names) {
        for (int i = par->minimum.i; i <= par->maximum.i; i++) {
            if (!strcmp(value, par->names[i])) {
                out->i = i;
                return 0;
            }
        }
    }
    errno = 0;
    switch (par->type) {
    case PL_VAR_SINT: {
        long num = strtol(value, &tail, 0);

        if (tail == value || *tail || errno || num < INT_MIN || num > INT_MAX) {
            return AVERROR(EINVAL);
        }
        if (num < par->minimum.i || num > par->maximum.i) {
            return AVERROR(ERANGE);
        }
        out->i = (int)num;
        return 0;
    }
    case PL_VAR_UINT: {
        unsigned long num = strtoul(value, &tail, 0);

        if (tail == value || *tail || errno ||
            value[strspn(value, " \t")] == '-' || num > UINT_MAX) {
            return AVERROR(EINVAL);
        }
        if (num < par->minimum.u || num > par->maximum.u) {
            return AVERROR(ERANGE);
        }
        out->u = (unsigned)num;
        return 0;
    }
    case PL_VAR_FLOAT: {
        double num = av_strtod(value, &tail);

        if (tail == value || *tail || isnan(num) || isinf(num)) {
            return AVERROR(EINVAL);
        }
        if (num < (double)par->minimum.f || num > (double)par->maximum.f) {
            return AVERROR(ERANGE);
        }
        out->f = (float)num;
        return 0;
    }
    default:
        return AVERROR(EINVAL);
    }
}

static void apply_params(const struct pl_hook *hook, int shader, uint8_t *used) {
    const AVDictionaryEntry *entry = NULL;
    char stem[256];
    int n = 0;

    shader_stem(shader, stem, sizeof(stem));
    while ((entry = av_dict_iterate(params, entry))) {
        const char *name = entry->key;
        const char *slash = strchr(name, '/');
        int index = n++;

        if (slash) {
            size_t len = (size_t)(slash - name);

            if (strncmp(name, stem, len) || stem[len]) {
                continue;
            }
            name = slash + 1;
        }
        for (int i = 0; i < hook->num_parameters; i++) {
            const struct pl_hook_par *par = &hook->parameters[i];
            pl_var_data data;
            char range[64];
            int ret;

            if (strcmp(par->name, name)) {
                continue;
            }
            used[index] = 1;
            ret = parse_value(par, entry->value, &data);
            if (ret == AVERROR(ERANGE)) {
                log_warn("-shader-params: %s must be between %s, not %s.\n",
                         entry->key, describe_range(par, range, sizeof(range)),
                         entry->value);
            } else if (ret < 0) {
                log_warn("-shader-params: '%s' is not a value for %s.\n",
                         entry->value, entry->key);
            } else {
                *par->data = data;
            }
            break;
        }
    }
}

static void report_unused_params(const uint8_t *used) {
    const AVDictionaryEntry *entry = NULL;
    int n = 0;

    while ((entry = av_dict_iterate(params, entry))) {
        const char *slash = strchr(entry->key, '/');

        if (used[n++]) {
            continue;
        }
        if (slash) {
            log_warn("-shader-params: no shader named '%.*s' has a parameter "
                     "named '%s'.\n",
                     (int)(slash - entry->key), entry->key, slash + 1);
        } else {
            log_warn("-shader-params: no shader has a parameter named '%s'.\n",
                     entry->key);
        }
    }
}

typedef struct ShaderSlot {
    const struct pl_hook *hook;
    int failed;
} ShaderSlot;

struct ShaderSet {
    ShaderSlot slots[SHADER_MAX];
    int num;
};

ShaderSet *shader_set_load(const struct pl_gpu_t *gpu) {
    ShaderSet *set = av_mallocz(sizeof(*set));
    int num_params = av_dict_count(params);
    uint8_t *used = NULL;
    enum pl_log_level level = gpu->log ? gpu->log->params.log_level : PL_LOG_NONE;

    if (!set) {
        return NULL;
    }
    if (num_params > 0 && !(used = av_calloc((size_t)num_params, 1))) {
        av_free(set);
        return NULL;
    }
    if (gpu->log && level < PL_LOG_WARN) {
        pl_log_level_update(gpu->log, PL_LOG_WARN);
    }
    set->num = num_paths;
    for (int i = 0; i < num_paths; i++) {
        ShaderSlot *slot = &set->slots[i];
        char *text = NULL;
        size_t len = 0;
        int ret = read_file(paths[i], &text, &len);

        if (ret < 0) {
            log_warn("Failed to read the shader '%s': %s.\n", paths[i],
                     av_err2str(ret));
            slot->failed = 1;
            continue;
        }
        if (!len) {
            log_warn("The shader '%s' is empty.\n", paths[i]);
            av_free(text);
            slot->failed = 1;
            continue;
        }
        slot->hook = pl_mpv_user_shader_parse(gpu, text, len);
        av_free(text);
        if (!slot->hook) {
            log_warn("The shader '%s' could not be parsed.\n", paths[i]);
            slot->failed = 1;
            continue;
        }
        if (used) {
            apply_params(slot->hook, i, used);
        }
        log_verbose("Loaded the shader '%s'.\n", paths[i]);
    }
    if (gpu->log && level < PL_LOG_WARN) {
        pl_log_level_update(gpu->log, level);
    }
    if (used) {
        report_unused_params(used);
        av_free(used);
    }

    return set;
}

void shader_set_free(ShaderSet **set) {
    if (!*set) {
        return;
    }
    for (int i = 0; i < (*set)->num; i++) {
        pl_mpv_user_shader_destroy(&(*set)->slots[i].hook);
    }
    av_freep(set);
}

int shader_set_hooks(const ShaderSet *set, const struct pl_hook **hooks,
                     int max) {
    int num = 0;

    for (int i = 0; i < set->num && num < max; i++) {
        if (set->slots[i].hook && !set->slots[i].failed) {
            hooks[num++] = set->slots[i].hook;
        }
    }

    return num;
}

void shader_set_note_disabled(ShaderSet *set, const uint64_t *signatures,
                              int num) {
    for (int i = 0; i < set->num; i++) {
        ShaderSlot *slot = &set->slots[i];

        if (!slot->hook || slot->failed) {
            continue;
        }
        for (int j = 0; j < num; j++) {
            if (signatures[j] == slot->hook->signature) {
                log_warn("The shader '%s' failed to run.\n", paths[i]);
                slot->failed = 1;
                break;
            }
        }
    }
}

int shader_set_num_failed(const ShaderSet *set) {
    int failed = 0;

    for (int i = 0; i < set->num; i++) {
        failed += set->slots[i].failed;
    }

    return failed;
}

const char *shader_status(enum ShaderState state, int num_failed) {
    static char status[256];

    if (!num_paths) {
        return "none";
    }
    status[0] = '\0';
    for (int i = 0; i < num_paths; i++) {
        av_strlcatf(status, sizeof(status), "%s%s", i ? ", " : "",
                    shader_name(i));
    }
    switch (state) {
    case SHADER_NO_RENDERER:
        av_strlcat(status, ", no renderer to run them", sizeof(status));
        break;
    case SHADER_BENCHMARKING:
        av_strlcat(status, ", suspended while benchmarking", sizeof(status));
        break;
    case SHADER_OFF:
        av_strlcat(status, ", off", sizeof(status));
        break;
    default:
        if (num_failed > 0) {
            av_strlcatf(status, sizeof(status), ", %d failed", num_failed);
        }
        break;
    }

    return status;
}
