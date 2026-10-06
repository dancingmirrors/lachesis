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
#include <stdint.h>
#include <string.h>

#include <libavutil/error.h>

#include "lachesis_alloc.h"
#include "lachesis_icc.h"

#define ICC_HEADER_SIZE 128
#define ICC_TAG_ENTRY_SIZE 12

#define ICC_SIG_ACSP 0x61637370u /* 'acsp' */
#define ICC_SIG_VCGT 0x76636774u /* 'vcgt' */

#define ICC_RAMP_MAX 4096
#define ICC_RAMP_FORMULA 256

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}

static unsigned rd16(const uint8_t *p) {
    return ((unsigned)p[0] << 8) | p[1];
}

/* s15Fixed16Number, the fixed point format ICC uses for everything numeric. */
static double rd_s15f16(const uint8_t *p) {
    return (double)(int32_t)rd32(p) / 65536.0;
}

static void sig_to_name(uint32_t sig, char out[5]) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)(sig >> (24 - 8 * i));

        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[4] = '\0';
}

static const uint8_t *find_tag(const uint8_t *data, size_t len, uint32_t sig,
                               size_t *tag_len) {
    uint32_t count;

    if (len < ICC_HEADER_SIZE + 4) {
        return NULL;
    }
    count = rd32(data + ICC_HEADER_SIZE);
    if (count > (len - ICC_HEADER_SIZE - 4) / ICC_TAG_ENTRY_SIZE) {
        return NULL;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = data + ICC_HEADER_SIZE + 4 + i * ICC_TAG_ENTRY_SIZE;
        uint32_t offset, size;

        if (rd32(entry) != sig) {
            continue;
        }
        offset = rd32(entry + 4);
        size = rd32(entry + 8);
        if (offset > len || size > len - offset) {
            return NULL;
        }
        *tag_len = size;

        return data + offset;
    }

    return NULL;
}

int icc_profile_inspect(const void *data, size_t len, IccProfileInfo *info,
                        const char **why) {
    const uint8_t *p = data;
    uint32_t claimed;
    size_t vcgt_len;

    memset(info, 0, sizeof(*info));
    *why = "";

    if (!p || len < ICC_HEADER_SIZE + 4) {
        *why = "the file is too small to be an ICC profile";
        return AVERROR_INVALIDDATA;
    }
    if (rd32(p + 36) != ICC_SIG_ACSP) {
        *why = "the file does not carry the 'acsp' ICC signature";
        return AVERROR_INVALIDDATA;
    }

    claimed = rd32(p);
    if (claimed > len) {
        *why = "the profile is truncated";
        return AVERROR_INVALIDDATA;
    }

    sig_to_name(rd32(p + 12), info->device_class);
    sig_to_name(rd32(p + 16), info->color_space);
    info->version_major = p[8];
    info->version_minor = p[9] >> 4;

    if (memcmp(info->color_space, "RGB ", 4)) {
        *why = "the profile is not an RGB profile";
        return AVERROR_INVALIDDATA;
    }

    /* Too short to hold even a type signature is as good as absent. */
    info->has_vcgt = find_tag(p, len, ICC_SIG_VCGT, &vcgt_len) && vcgt_len >= 12;

    return 0;
}

static int vcgt_from_table(const uint8_t *tag, size_t tag_len,
                           IccGammaRamp *ramp) {
    unsigned channels, entries, entry_size, want;
    const uint8_t *src;
    float scale;
    float *out;

    if (tag_len < 18) {
        return AVERROR_INVALIDDATA;
    }
    channels = rd16(tag + 12);
    entries = rd16(tag + 14);
    entry_size = rd16(tag + 16);
    src = tag + 18;

    if (channels != 3 || entries < 2 || (entry_size != 1 && entry_size != 2)) {
        return AVERROR_INVALIDDATA;
    }
    if (tag_len - 18 < (size_t)channels * entries * entry_size) {
        return AVERROR_INVALIDDATA;
    }

    /* A ramp longer than this would be a 1D texture no GPU has to support. */
    want = entries > ICC_RAMP_MAX ? ICC_RAMP_MAX : entries;

    out = av_malloc_array(want, 3 * sizeof(*out));
    if (!out) {
        return AVERROR(ENOMEM);
    }
    scale = entry_size == 1 ? 1.0f / 255.0f : 1.0f / 65535.0f;

    for (unsigned c = 0; c < 3; c++) {
        for (unsigned i = 0; i < want; i++) {
            double at = (double)i * (entries - 1) / (want - 1);
            unsigned lo = (unsigned)at;
            unsigned hi = lo + 1 < entries ? lo + 1 : lo;
            const uint8_t *a = src + (c * entries + lo) * entry_size;
            const uint8_t *b = src + (c * entries + hi) * entry_size;
            double va = entry_size == 1 ? *a : rd16(a);
            double vb = entry_size == 1 ? *b : rd16(b);

            out[i * 3 + c] = (float)(va + (vb - va) * (at - lo)) * scale;
        }
    }

    ramp->size = (int)want;
    ramp->data = out;

    return 0;
}

static int vcgt_from_formula(const uint8_t *tag, size_t tag_len,
                             IccGammaRamp *ramp) {
    double gamma[3], min[3], max[3];
    float *out;

    if (tag_len < 12 + 9 * 4) {
        return AVERROR_INVALIDDATA;
    }
    for (int c = 0; c < 3; c++) {
        gamma[c] = rd_s15f16(tag + 12 + c * 12);
        min[c] = rd_s15f16(tag + 16 + c * 12);
        max[c] = rd_s15f16(tag + 20 + c * 12);
        if (!(gamma[c] > 0.0) || !(max[c] > min[c])) {
            return AVERROR_INVALIDDATA;
        }
    }

    out = av_malloc_array(ICC_RAMP_FORMULA, 3 * sizeof(*out));
    if (!out) {
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < ICC_RAMP_FORMULA; i++) {
        double x = (double)i / (ICC_RAMP_FORMULA - 1);

        for (int c = 0; c < 3; c++) {
            out[i * 3 + c] = (float)(min[c] + (max[c] - min[c]) * pow(x, gamma[c]));
        }
    }

    ramp->size = ICC_RAMP_FORMULA;
    ramp->data = out;

    return 0;
}

int icc_profile_read_vcgt(const void *data, size_t len, IccGammaRamp *ramp) {
    const uint8_t *tag;
    size_t tag_len = 0;
    int ret;

    memset(ramp, 0, sizeof(*ramp));

    tag = find_tag(data, len, ICC_SIG_VCGT, &tag_len);
    if (!tag) {
        return AVERROR(ENOENT);
    }
    if (tag_len < 12 || rd32(tag) != ICC_SIG_VCGT) {
        return AVERROR_INVALIDDATA;
    }

    switch (rd32(tag + 8)) {
    case 0:
        ret = vcgt_from_table(tag, tag_len, ramp);
        break;
    case 1:
        ret = vcgt_from_formula(tag, tag_len, ramp);
        break;
    default:
        return AVERROR_INVALIDDATA;
    }
    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < ramp->size * 3; i++) {
        float v = ramp->data[i];

        if (!(v >= 0.0f)) {
            v = 0.0f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }
        ramp->data[i] = v;
    }

    return 0;
}

int icc_gamma_ramp_identity(int size, IccGammaRamp *ramp) {
    float *out;

    memset(ramp, 0, sizeof(*ramp));
    if (size < 2) {
        return AVERROR(EINVAL);
    }
    out = av_malloc_array((size_t)size, 3 * sizeof(*out));
    if (!out) {
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < size; i++) {
        float x = (float)i / (size - 1);

        out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = x;
    }
    ramp->size = size;
    ramp->data = out;

    return 0;
}

void icc_gamma_ramp_scale(IccGammaRamp *ramp, const float scale[3]) {
    if (!ramp->data) {
        return;
    }
    for (int i = 0; i < ramp->size; i++) {
        for (int c = 0; c < 3; c++) {
            float v = ramp->data[i * 3 + c] * scale[c];

            ramp->data[i * 3 + c] = v > 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f);
        }
    }
}

int icc_gamma_ramp_is_identity(const IccGammaRamp *ramp) {
    if (!ramp->data || ramp->size < 2) {
        return 1;
    }

    for (int i = 0; i < ramp->size; i++) {
        float x = (float)i / (ramp->size - 1);

        for (int c = 0; c < 3; c++) {
            if (fabsf(ramp->data[i * 3 + c] - x) > 1.0f / 512.0f) {
                return 0;
            }
        }
    }

    return 1;
}

void icc_gamma_ramp_free(IccGammaRamp *ramp) {
    av_freep(&ramp->data);
    ramp->size = 0;
}
