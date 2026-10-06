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

/* clang-format off */
#include "lachesis_alloc.h"
#include "lachesis_config.h"
#include "lachesis_icc.h"
#include "lachesis_log.h"
#include "lachesis_renderer.h"
#include "lachesis_renderer_internal.h"
/* clang-format on */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/colorspace.h>

#include <libavutil/dict.h>
#include <libavutil/mem.h>

static const char *icc_intent_name(enum pl_rendering_intent intent) {
    switch (intent) {
    case PL_INTENT_PERCEPTUAL:
        return "perceptual";
    case PL_INTENT_RELATIVE_COLORIMETRIC:
        return "relative colorimetric";
    case PL_INTENT_SATURATION:
        return "saturation";
    case PL_INTENT_ABSOLUTE_COLORIMETRIC:
        return "absolute colorimetric";
    default:
        return "the profile's own";
    }
}

void icc_forget(RendererContext *ctx) {
    pl_icc_close(&ctx->icc_obj);
    av_freep(&ctx->icc_data);
    ctx->icc_len = 0;
    ctx->icc_sig = 0;
    ctx->icc_vcgt_loaded = 0;
}

void cal_drop(RendererContext *ctx) {
    icc_gamma_ramp_free(&ctx->cal_ramp);
    ctx->cal_lut = (struct pl_custom_lut){0};
}

#define DISPLAY_GAMMA 2.2f
#define CAL_RAMP_SIZE 256

static double white_point_gains(double kelvin, float gain[3]) {
    const struct pl_raw_primaries *prim =
        pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    struct pl_cie_xy white;
#if PL_API_VER < 357
    if (kelvin < 2500.0) {
        kelvin = 2500.0;
    }
#endif
    white = pl_white_from_temp((float)kelvin);
    pl_matrix3x3 xyz2rgb = pl_get_xyz2rgb_matrix(prim);
    float rgb[3];
    float top;

    rgb[0] = white.x / white.y;
    rgb[1] = 1.0f;
    rgb[2] = (1.0f - white.x - white.y) / white.y;
    pl_matrix3x3_apply(&xyz2rgb, rgb);

    top = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
    for (int i = 0; i < 3; i++) {
        gain[i] = top > 0.0f ? fmaxf(rgb[i] / top, 0.0f) : 1.0f;
    }

    return kelvin;
}

static int cal_read_vcgt(RendererContext *ctx) {
    int ret = icc_profile_read_vcgt(ctx->icc_data, ctx->icc_len, &ctx->cal_ramp);

    if (ret == AVERROR(ENOENT)) {
        log_verbose("The ICC profile has no calibration curves.\n");
        return 0;
    }
    if (ret < 0) {
        log_warn("Ignoring the unreadable calibration curves in the ICC profile.\n");
        cal_drop(ctx);
        return 0;
    }
    log_verbose("Using the ICC profile's calibration curves (%d entries).\n",
                ctx->cal_ramp.size);

    return 1;
}

static void cal_build(RendererContext *ctx) {
    float scale[3] = {1.0f, 1.0f, 1.0f};
    uint64_t sig;

    cal_drop(ctx);
    ctx->icc_vcgt_loaded = 0;

    if (ctx->icc_vcgt && ctx->icc_data) {
        ctx->icc_vcgt_loaded = cal_read_vcgt(ctx);
    }
    if (ctx->white_point > 0.0) {
        float gamma = DISPLAY_GAMMA;
        float gain[3];

        if (ctx->icc_obj && ctx->icc_obj->gamma >= 1.0f &&
            ctx->icc_obj->gamma <= 3.0f) {
            gamma = ctx->icc_obj->gamma;
        }
        double applied = white_point_gains(ctx->white_point, gain);

        for (int i = 0; i < 3; i++) {
            scale[i] = powf(gain[i], 1.0f / gamma);
        }
        if (!ctx->cal_ramp.data &&
            icc_gamma_ramp_identity(CAL_RAMP_SIZE, &ctx->cal_ramp) < 0) {
            return;
        }
        log_verbose("Adapting the picture to a white point of %.0f K "
                    "(red %.3f, green %.3f, blue %.3f).\n",
                    applied, (double)gain[0], (double)gain[1],
                    (double)gain[2]);
        if (applied != ctx->white_point) {
            log_warn("This libplacebo cannot go below %.0f K, so the requested "
                     "%.0f K was not applied.\n",
                     applied, ctx->white_point);
        }
    }
    if (!ctx->cal_ramp.data) {
        return;
    }

    icc_gamma_ramp_scale(&ctx->cal_ramp, scale);
    if (icc_gamma_ramp_is_identity(&ctx->cal_ramp)) {
        log_verbose("The display correction does nothing.\n");
        cal_drop(ctx);
        ctx->icc_vcgt_loaded = 0;
        return;
    }

    sig = ctx->icc_sig ^ UINT64_C(0x7663677476636774);
    sig ^= (uint64_t)(ctx->white_point * 16.0) * UINT64_C(0x9e3779b97f4a7c15);
    ctx->cal_lut = (struct pl_custom_lut){
        .signature = sig,
        .size = {ctx->cal_ramp.size, 0, 0},
        .data = ctx->cal_ramp.data,
    };
}

static void icc_build(RendererContext *ctx) {
    struct pl_icc_profile profile = {
        .data = ctx->icc_data,
        .len = ctx->icc_len,
        .signature = ctx->icc_sig,
    };

    pl_icc_close(&ctx->icc_obj);

    if (!ctx->icc_data) {
        cal_build(ctx);
        return;
    }

    if (!pl_icc_update(ctx->log_ctx, &ctx->icc_obj, &profile,
                       pl_icc_params(.intent = ctx->icc_intent))) {
#ifdef PL_HAVE_LCMS
        log_warn("Could not open the ICC profile.\n");
#else
        log_warn("No PL_HAVE_LCMS means no ICC profile support.\n");
#endif
        ctx->icc_obj = NULL;
    } else {
        log_verbose("ICC profile: %s primaries, gamma %.2f, %.0f cd/m² white, %.4f cd/m² black, %s intent\n",
                    pl_color_primaries_name(ctx->icc_obj->containing_primaries),
                    (double)ctx->icc_obj->gamma,
                    (double)ctx->icc_obj->csp.hdr.max_luma,
                    (double)ctx->icc_obj->csp.hdr.min_luma,
                    icc_intent_name(ctx->icc_obj->params.intent));
    }

    cal_build(ctx);

    if (!ctx->icc_obj && !ctx->icc_vcgt_loaded) {
        log_warn("The ICC profile has no effect.\n");
        icc_forget(ctx);
    }
}

void icc_track_luma(RendererContext *ctx, float max_luma) {
    if (!ctx->icc_obj || max_luma <= 0.0f ||
        max_luma == ctx->icc_obj->params.max_luma) {
        return;
    }
    if (!pl_icc_update(ctx->log_ctx, &ctx->icc_obj, NULL,
                       pl_icc_params(.intent = ctx->icc_intent,
                                     .max_luma = max_luma))) {
        log_warn("The ICC profile could not be reopened for a %.0f cd/m² "
                 "display, so it is no longer applied.\n",
                 (double)max_luma);
        ctx->icc_obj = NULL;
    }
}

static int icc_adopt(RendererContext *ctx, void *data, size_t len) {
    struct pl_icc_profile profile;

    if (!data || !len) {
        av_free(data);
        return 0;
    }

    profile = (struct pl_icc_profile){.data = data, .len = len};
    pl_icc_profile_compute_signature(&profile);

    if (ctx->icc_data && ctx->icc_sig == profile.signature) {
        av_free(data);
        return 0;
    }

    av_freep(&ctx->icc_data);
    ctx->icc_data = data;
    ctx->icc_len = len;
    ctx->icc_sig = profile.signature;
    icc_build(ctx);

    return 1;
}

static int icc_check(const void *data, size_t len, const char *what,
                     int *has_vcgt) {
    IccProfileInfo info;
    const char *why;

    *has_vcgt = 0;
    if (icc_profile_inspect(data, len, &info, &why) < 0) {
        log_warn("%s is not usable: %s.\n", what, why);
        return 0;
    }
    log_verbose("%s is a v%d.%d '%s' profile in the '%s' color space%s.\n", what,
                info.version_major, info.version_minor, info.device_class,
                info.color_space,
                info.has_vcgt ? " with calibration curves" : "");
    *has_vcgt = info.has_vcgt;

    return 1;
}

static int icc_load_file(RendererContext *ctx, const char *path) {
    long size;
    void *data;
    FILE *f;
    int has_vcgt = 0;

    f = fopen(path, "rb");
    if (!f) {
        log_warn("Failed to open ICC profile '%s'.\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        log_warn("Failed to read ICC profile '%s'.\n", path);
        fclose(f);
        return 0;
    }
    data = av_malloc((size_t)size);
    if (!data) {
        fclose(f);
        return 0;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        log_warn("Failed to read ICC profile '%s'.\n", path);
        av_free(data);
        fclose(f);
        return 0;
    }
    fclose(f);

    if (!icc_check(data, (size_t)size, "The ICC profile", &has_vcgt)) {
        av_free(data);
        return 0;
    }
    if (has_vcgt && !ctx->icc_vcgt) {
        log_info("The ICC profile carries calibration curves. Pass -icc-vcgt "
                 "to apply them.\n");
    }
    if (!icc_adopt(ctx, data, (size_t)size)) {
        return 0;
    }
    if (ctx->icc_data) {
        log_info("Loaded ICC profile: %s\n", path);
    }

    return 1;
}

int icc_load_display(RendererContext *ctx, SDL_Window *window) {
    size_t size = 0;
    void *sdl_data;
    void *data;
    int has_vcgt = 0;

    if (!ctx->icc_auto || ctx->icc_from_file || !window) {
        return 0;
    }

    sdl_data = SDL_GetWindowICCProfile(window, &size);
    if (!sdl_data || !size) {
        SDL_free(sdl_data);
        if (!ctx->icc_data) {
            log_verbose("The display advertises no ICC profile.\n");
            return 0;
        }
        icc_forget(ctx);
        cal_build(ctx);
        log_verbose("The display no longer advertises an ICC profile.\n");
        return 1;
    }
    data = av_memdup(sdl_data, size);
    SDL_free(sdl_data);
    if (!data) {
        return 0;
    }

    if (!icc_check(data, size, "The display's ICC profile", &has_vcgt)) {
        av_free(data);
        if (!ctx->icc_data) {
            return 0;
        }
        icc_forget(ctx);
        cal_build(ctx);
        return 1;
    }
    if (has_vcgt && !ctx->icc_vcgt) {
        log_info("The display's ICC profile carries calibration curves. Pass "
                 "-icc-vcgt to apply them.\n");
    }
    if (!icc_adopt(ctx, data, size)) {
        return 0;
    }
    log_verbose("Using the display's ICC profile (%llu bytes).\n",
                (unsigned long long)size);

    return 1;
}

static enum pl_rendering_intent icc_parse_intent(const AVDictionary *opt) {
    static const struct {
        const char *name;
        enum pl_rendering_intent intent;
    } intents[] = {
        {"auto", PL_INTENT_AUTO},
        {"perceptual", PL_INTENT_PERCEPTUAL},
        {"relative", PL_INTENT_RELATIVE_COLORIMETRIC},
        {"saturation", PL_INTENT_SATURATION},
        {"absolute", PL_INTENT_ABSOLUTE_COLORIMETRIC},
    };
    const AVDictionaryEntry *entry = av_dict_get(opt, "icc_intent", NULL, 0);

    if (!entry || !entry->value) {
        return PL_INTENT_RELATIVE_COLORIMETRIC;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(intents); i++) {
        if (!strcmp(entry->value, intents[i].name)) {
            return intents[i].intent;
        }
    }

    return PL_INTENT_RELATIVE_COLORIMETRIC;
}

static double white_point_wanted(const AVDictionary *opt) {
    const AVDictionaryEntry *entry =
        av_dict_get(opt, "color_temperature", NULL, 0);

    return entry && entry->value ? strtod(entry->value, NULL) : 0.0;
}

void icc_setup(RendererContext *ctx, SDL_Window *window,
               const AVDictionary *opt) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "icc_vcgt", NULL, 0);

    ctx->icc_vcgt = entry && strtol(entry->value, NULL, 10);
    ctx->icc_intent = icc_parse_intent(opt);
    ctx->white_point = white_point_wanted(opt);

    entry = av_dict_get(opt, "icc_profile", NULL, 0);
    if (entry && entry->value && entry->value[0]) {
        ctx->icc_from_file = 1;
        if (!icc_load_file(ctx, entry->value)) {
            cal_build(ctx);
        }
        return;
    }

    entry = av_dict_get(opt, "icc_auto", NULL, 0);
    ctx->icc_auto = entry && strtol(entry->value, NULL, 10);
    if (!icc_load_display(ctx, window)) {
        cal_build(ctx);
    }
}

#define SDL_SCRGB_NITS 80.0f

int hdr_refresh(RendererContext *ctx, SDL_Window *window) {
    struct pl_hdr_metadata hdr = {0};
    SDL_PropertiesID props;
    float headroom, sdr_white, max_luma;

    if (!ctx->hdr_auto || !window) {
        return 0;
    }

    props = SDL_GetWindowProperties(window);
    if (!props) {
        return 0;
    }
    if (!SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) {
        goto done;
    }

    headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    if (!(headroom > 1.0f)) {
        goto done;
    }

    sdr_white = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    sdr_white = sdr_white == 1.0f ? PL_COLOR_SDR_WHITE : sdr_white * SDL_SCRGB_NITS;

    max_luma = sdr_white * headroom;
    if (!(max_luma >= 100.0f) || max_luma > 100000.0f) {
        if (!ctx->hdr_warned) {
            ctx->hdr_warned = 1;
            log_warn("Ignoring an implausible display peak of %.1f cd/m².\n",
                     (double)max_luma);
        }
        goto done;
    }
    hdr.max_luma = max_luma;

done:
    if (pl_hdr_metadata_equal(&hdr, &ctx->display_hdr)) {
        return 0;
    }
    ctx->display_hdr = hdr;
    ctx->have_display_hdr = hdr.max_luma > 0;
    if (ctx->have_display_hdr) {
        log_verbose("The display reports a peak of %.1f cd/m².\n",
                    (double)hdr.max_luma);
    } else {
        log_verbose("The display no longer reports HDR metadata.\n");
    }

    return 1;
}
