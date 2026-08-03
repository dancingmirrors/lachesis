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

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#include "lachesis_equalizer.h"
#include "lachesis_log.h"
#include "lachesis_osd.h"

#define EQ_MAX_DEPTH 8
#define EQ_LUT_SIZE (1 << EQ_MAX_DEPTH)
#define EQ_MAX_PERIOD 4

static EqualizerValues eq_values;
static unsigned eq_gen = 1;

static struct {
    unsigned gen;
    int format;
    int full_range;
    int valid;
    uint16_t lut[3][EQ_LUT_SIZE];
    uint16_t identity[EQ_LUT_SIZE];
} eq_lut;

static AVFrame *eq_scratch;
static uint16_t *eq_line;
static int eq_line_size;

void equalizer_uninit(void) {
    av_frame_free(&eq_scratch);
    av_freep(&eq_line);
    eq_line_size = 0;
    eq_lut.valid = 0;
}

EqualizerValues equalizer_get(void) {
    return eq_values;
}

unsigned equalizer_generation(void) {
    return eq_gen;
}

float equalizer_pl_brightness(int brightness) {
    return brightness / 100.0f;
}

float equalizer_pl_contrast(int contrast) {
    return (contrast + 100) / 100.0f;
}

float equalizer_pl_gamma(int gamma) {
    return (float)exp(log(8.0) * gamma / 100.0);
}

void equalizer_adjust(enum EqualizerControl control, int steps) {
    const char *name;
    int *value;

    switch (control) {
    case EQ_BRIGHTNESS:
        name = "Brightness";
        value = &eq_values.brightness;
        break;
    case EQ_GAMMA:
        name = "Gamma";
        value = &eq_values.gamma;
        break;
    case EQ_CONTRAST:
        name = "Contrast";
        value = &eq_values.contrast;
        break;
    default:
        return;
    }

    int adjusted = av_clip(*value + steps, EQ_VALUE_MIN, EQ_VALUE_MAX);
    if (adjusted != *value) {
        *value = adjusted;
        eq_gen++;
    }
    osd_show_message("%s: %d", name, *value);
}

static void eq_build_signal_lut(uint16_t *lut, int size, int black, int range,
                                float brightness, float contrast, float gamma) {
    double inv_gamma = 1.0 / gamma;

    for (int i = 0; i < size; i++) {
        double v = (i - black) / (double)range;

        v = contrast * v + brightness;
        if (gamma != 1.0f) {
            v = pow(v < 0.0 ? 0.0 : v, inv_gamma);
        }
        lut[i] = av_clip((int)lrint(black + v * range), 0, size - 1);
    }
}

static void eq_build_chroma_lut(uint16_t *lut, int size, float contrast) {
    int mid = size / 2;

    for (int i = 0; i < size; i++) {
        lut[i] = av_clip((int)lrint(mid + contrast * (i - mid)), 0, size - 1);
    }
}

static int eq_update_luts(const AVPixFmtDescriptor *desc, const AVFrame *frame,
                          EqualizerValues values) {
    int is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    int full_range = is_rgb || frame->color_range == AVCOL_RANGE_JPEG;
    float brightness = equalizer_pl_brightness(values.brightness);
    float contrast = equalizer_pl_contrast(values.contrast);
    float gamma = equalizer_pl_gamma(values.gamma);

    if (eq_lut.valid && eq_lut.gen == eq_gen && eq_lut.format == frame->format &&
        eq_lut.full_range == full_range) {
        return 1;
    }

    for (int c = 0; c < 3; c++) {
        int size = 1 << desc->comp[c].depth;

        if (is_rgb || c == 0) {
            int black = (is_rgb || full_range) ? 0 : 16 * size / 256;
            int range = (is_rgb || full_range) ? size - 1 : 219 * size / 256;

            eq_build_signal_lut(eq_lut.lut[c], size, black, range, brightness,
                                contrast, gamma);
        } else {
            eq_build_chroma_lut(eq_lut.lut[c], size, contrast);
        }
    }

    eq_lut.gen = eq_gen;
    eq_lut.format = frame->format;
    eq_lut.full_range = full_range;
    eq_lut.valid = 1;

    return 1;
}

static int eq_update_scratch(const AVFrame *frame) {
    if (eq_scratch && (eq_scratch->format != frame->format || eq_scratch->width != frame->width || eq_scratch->height != frame->height)) {
        av_frame_free(&eq_scratch);
    }
    if (!eq_scratch) {
        eq_scratch = av_frame_alloc();
        if (!eq_scratch) {
            return 0;
        }
        eq_scratch->format = frame->format;
        eq_scratch->width = frame->width;
        eq_scratch->height = frame->height;
        if (av_frame_get_buffer(eq_scratch, 0) < 0) {
            av_frame_free(&eq_scratch);
            return 0;
        }
    }

    return 1;
}

static void eq_init_identity(void) {
    static int done;

    if (!done) {
        for (int i = 0; i < EQ_LUT_SIZE; i++) {
            eq_lut.identity[i] = i;
        }
        done = 1;
    }
}

static int eq_apply_bytes(AVFrame *dst, const AVFrame *src,
                          const AVPixFmtDescriptor *desc) {
    const uint16_t *pattern[AV_NUM_DATA_POINTERS][EQ_MAX_PERIOD];
    int period[AV_NUM_DATA_POINTERS];
    int nb_planes = av_pix_fmt_count_planes(dst->format);

    if (nb_planes <= 0 || nb_planes > AV_NUM_DATA_POINTERS) {
        return 0;
    }
    eq_init_identity();
    for (int c = 0; c < 3; c++) {
        if (desc->comp[c].depth != EQ_MAX_DEPTH || desc->comp[c].shift) {
            return 0;
        }
    }

    for (int p = 0; p < nb_planes; p++) {
        int row_bytes = av_image_get_linesize(dst->format, dst->width, p);

        if (!src->data[p] || !dst->data[p] || row_bytes <= 0) {
            return 0;
        }
        period[p] = 1;
        for (int c = 0; c < 3; c++) {
            if (desc->comp[c].plane == p && desc->comp[c].step > period[p]) {
                period[p] = desc->comp[c].step;
            }
        }
        if (period[p] > EQ_MAX_PERIOD || row_bytes % period[p]) {
            return 0;
        }
        for (int i = 0; i < period[p]; i++) {
            pattern[p][i] = eq_lut.identity;
        }
        for (int c = 0; c < 3; c++) {
            const AVComponentDescriptor *comp = &desc->comp[c];

            if (comp->plane != p) {
                continue;
            }
            for (int i = comp->offset; i < period[p]; i += comp->step) {
                pattern[p][i] = eq_lut.lut[c];
            }
        }
    }

    for (int p = 0; p < nb_planes; p++) {
        int row_bytes = av_image_get_linesize(dst->format, dst->width, p);
        int rows = dst->height;

        if (desc->comp[0].plane != p &&
            (desc->comp[1].plane == p || desc->comp[2].plane == p)) {
            rows = AV_CEIL_RSHIFT(dst->height, desc->log2_chroma_h);
        }

        for (int y = 0; y < rows; y++) {
            const uint8_t *s = src->data[p] + (ptrdiff_t)y * src->linesize[p];
            uint8_t *d = dst->data[p] + (ptrdiff_t)y * dst->linesize[p];

            for (int x = 0; x < row_bytes; x += period[p]) {
                for (int i = 0; i < period[p]; i++) {
                    d[x + i] = (uint8_t)pattern[p][i][s[x + i]];
                }
            }
        }
    }

    return 1;
}

static int eq_apply_lines(AVFrame *dst, const AVFrame *src,
                          const AVPixFmtDescriptor *desc) {
    const uint8_t *src_data[4] = {NULL};
    int nb_planes = av_pix_fmt_count_planes(dst->format);
    int row_bytes = av_image_get_linesize(dst->format, dst->width, 0);

    if (nb_planes != 1 || desc->nb_components != 3 || row_bytes <= 0 ||
        desc->log2_chroma_w || desc->log2_chroma_h) {
        return 0;
    }
    if (eq_line_size < dst->width) {
        av_freep(&eq_line);
        eq_line = av_malloc_array(dst->width, sizeof(*eq_line));
        if (!eq_line) {
            eq_line_size = 0;
            return 0;
        }
        eq_line_size = dst->width;
    }

    for (int y = 0; y < dst->height; y++) {
        memset(dst->data[0] + (ptrdiff_t)y * dst->linesize[0], 0, row_bytes);
    }

    src_data[0] = src->data[0];
    for (int c = 0; c < 3; c++) {
        for (int y = 0; y < dst->height; y++) {
            av_read_image_line2(eq_line, src_data, src->linesize, desc, 0, y, c,
                                dst->width, 0, sizeof(*eq_line));
            for (int x = 0; x < dst->width; x++) {
                eq_line[x] = eq_lut.lut[c][eq_line[x]];
            }
            av_write_image_line2(eq_line, dst->data, dst->linesize, desc, 0, y, c,
                                 dst->width, sizeof(*eq_line));
        }
    }

    return 1;
}

AVFrame *equalizer_apply(AVFrame *frame) {
    const AVPixFmtDescriptor *desc;
    EqualizerValues values = eq_values;
    int applied;

    if (equalizer_values_neutral(values) || !frame || frame->width <= 0 ||
        frame->height <= 0) {
        return frame;
    }

    desc = av_pix_fmt_desc_get(frame->format);
    if (!desc || desc->nb_components < 3 ||
        (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_FLOAT))) {
        return frame;
    }
    for (int c = 0; c < 3; c++) {
        if (desc->comp[c].depth > EQ_MAX_DEPTH) {
            return frame;
        }
    }

    if (!eq_update_luts(desc, frame, values) || !eq_update_scratch(frame)) {
        return frame;
    }

    applied = eq_apply_bytes(eq_scratch, frame, desc) ||
        eq_apply_lines(eq_scratch, frame, desc);
    if (!applied) {
        static int warned_format = AV_PIX_FMT_NONE;

        if (warned_format != frame->format) {
            warned_format = frame->format;
            log_verbose("No equalizer support for %s.\n", desc->name);
        }
        return frame;
    }

    return eq_scratch;
}
