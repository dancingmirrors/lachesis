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
#include <stdio.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/mathematics.h>

#include <SDL3/SDL.h>

#include "lachesis_audio.h"
#include "lachesis_internal.h"
#include "lachesis_normalize.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"

#define SUBS_PER_BLOCK 4
#define SUBS_PER_SHORT_TERM 30

#define LUFS_OFFSET (-0.691)
#define GATE_ABSOLUTE (-70.0)
#define GATE_RELATIVE (-10.0)

#define HIST_BINS 1000
#define HIST_STEP 0.1

#define CONFIDENCE_MIN_BLOCKS 20.0
#define CONFIDENCE_FULL_BLOCKS 100.0
#define ADAPT_SHARE 0.15

#define SLEW_SEEKING 8.0
#define SLEW_SETTLED 0.5
#define SLEW_TOGGLE 18.0

#define CEILING_DB (-1.0)
#define LIMIT_HOLD_S 0.050
#define LIMIT_ATTACK_S 0.0005
#define LIMIT_RELEASE_FAST_S 0.060
#define LIMIT_RELEASE_SLOW_S 0.800

#define MAX_CHANNELS 64

#define NO_MEASUREMENT INT32_MIN

typedef struct Biquad {
    double b0, b1, b2, a1, a2;
} Biquad;

typedef struct BiquadState {
    double x1, x2, y1, y2;
} BiquadState;

typedef struct Normalizer {
    int rate;
    int channels;
    double weight[MAX_CHANNELS];

    Biquad shelf;
    Biquad hpf;
    BiquadState shelf_state[MAX_CHANNELS];
    BiquadState hpf_state[MAX_CHANNELS];

    int sub_frames;
    int sub_fill;
    double sub_energy;

    double sub[SUBS_PER_SHORT_TERM];
    int sub_head;
    int sub_count;

    double hist_energy[HIST_BINS];
    uint32_t hist_count[HIST_BINS];
    double gated_energy;
    double gated_blocks;

    double gain;
    double peak_hold;
    double reduction;
} Normalizer;

static Normalizer nrm = {.gain = 1.0, .reduction = 1.0};

static SDL_AtomicInt normalize_on;
static SDL_AtomicInt normalize_toggling;
static SDL_AtomicInt reported_lufs = {NO_MEASUREMENT};
static SDL_AtomicInt reported_gain;
static SDL_AtomicInt reported_reduction;

void normalize_init(void) {
    SDL_SetAtomicInt(&normalize_on, normalize_audio != 0);
}

int normalize_enabled(void) {
    return SDL_GetAtomicInt(&normalize_on) != 0;
}

static double normalize_target_lufs(void) {
    return av_clipd(normalize_target, NORMALIZE_TARGET_MIN, NORMALIZE_TARGET_MAX) +
        av_clipd(normalize_gain, NORMALIZE_GAIN_MIN, NORMALIZE_GAIN_MAX);
}

static void design_k_weighting(int rate) {
    const double shelf_f0 = 1681.974450955533;
    const double shelf_gain = 3.999843853973347;
    const double shelf_q = 0.7071752369554196;
    const double hpf_f0 = 38.13547087602444;
    const double hpf_q = 0.5003270373238773;
    double k, vh, vb, a0;

    k = tan(M_PI * shelf_f0 / rate);
    vh = pow(10.0, shelf_gain / 20.0);
    vb = pow(vh, 0.4996667741545416);
    a0 = 1.0 + k / shelf_q + k * k;
    nrm.shelf.b0 = (vh + vb * k / shelf_q + k * k) / a0;
    nrm.shelf.b1 = 2.0 * (k * k - vh) / a0;
    nrm.shelf.b2 = (vh - vb * k / shelf_q + k * k) / a0;
    nrm.shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    nrm.shelf.a2 = (1.0 - k / shelf_q + k * k) / a0;

    k = tan(M_PI * hpf_f0 / rate);
    a0 = 1.0 + k / hpf_q + k * k;
    nrm.hpf.b0 = 1.0;
    nrm.hpf.b1 = -2.0;
    nrm.hpf.b2 = 1.0;
    nrm.hpf.a1 = 2.0 * (k * k - 1.0) / a0;
    nrm.hpf.a2 = (1.0 - k / hpf_q + k * k) / a0;
}

static double biquad_run(const Biquad *f, BiquadState *s, double x) {
    double y = f->b0 * x + f->b1 * s->x1 + f->b2 * s->x2 -
        f->a1 * s->y1 - f->a2 * s->y2;

    if (!(fabs(y) > 1e-20)) {
        y = 0.0;
    }

    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;

    return y;
}

static double channel_weight(enum AVChannel ch) {
    switch (ch) {
    case AV_CHAN_LOW_FREQUENCY:
    case AV_CHAN_LOW_FREQUENCY_2:
        return 0.0;
    case AV_CHAN_SIDE_LEFT:
    case AV_CHAN_SIDE_RIGHT:
    case AV_CHAN_BACK_LEFT:
    case AV_CHAN_BACK_RIGHT:
    case AV_CHAN_BACK_CENTER:
    case AV_CHAN_SURROUND_DIRECT_LEFT:
    case AV_CHAN_SURROUND_DIRECT_RIGHT:
        return 1.41;
    default:
        return 1.0;
    }
}

static void forget_measurement(void) {
    memset(nrm.shelf_state, 0, sizeof(nrm.shelf_state));
    memset(nrm.hpf_state, 0, sizeof(nrm.hpf_state));
    memset(nrm.sub, 0, sizeof(nrm.sub));
    memset(nrm.hist_energy, 0, sizeof(nrm.hist_energy));
    memset(nrm.hist_count, 0, sizeof(nrm.hist_count));
    nrm.sub_fill = 0;
    nrm.sub_energy = 0.0;
    nrm.sub_head = 0;
    nrm.sub_count = 0;
    nrm.gated_energy = 0.0;
    nrm.gated_blocks = 0.0;
    SDL_SetAtomicInt(&reported_lufs, NO_MEASUREMENT);
}

void normalize_reset(void) {
    forget_measurement();
    nrm.rate = 0;
    nrm.channels = 0;
    nrm.peak_hold = 0.0;
    nrm.reduction = 1.0;
}

static int configure(int rate, int nb_channels, const AVChannelLayout *ch_layout) {
    double weight[MAX_CHANNELS];
    int i;

    if (rate <= 0 || nb_channels <= 0 || nb_channels > MAX_CHANNELS) {
        return 0;
    }

    for (i = 0; i < nb_channels; i++) {
        enum AVChannel ch = ch_layout
            ? av_channel_layout_channel_from_index(ch_layout, (unsigned)i)
            : AV_CHAN_NONE;

        weight[i] = ch == AV_CHAN_NONE ? 1.0 : channel_weight(ch);
    }

    if (nrm.rate == rate && nrm.channels == nb_channels &&
        !memcmp(nrm.weight, weight, sizeof(weight[0]) * (size_t)nb_channels)) {
        return 1;
    }

    forget_measurement();
    nrm.rate = rate;
    nrm.channels = nb_channels;
    memcpy(nrm.weight, weight, sizeof(weight[0]) * (size_t)nb_channels);
    nrm.sub_frames = FFMAX(1, rate / 10);
    design_k_weighting(rate);

    return 1;
}

static int window_loudness(int subs, double *out) {
    double energy = 0.0;
    int n = FFMIN(subs, nrm.sub_count);
    int i;

    if (n < SUBS_PER_BLOCK) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        energy += nrm.sub[(nrm.sub_head - 1 - i + SUBS_PER_SHORT_TERM) % SUBS_PER_SHORT_TERM];
    }
    energy /= (double)n * nrm.sub_frames;
    if (!(energy > 0.0)) {
        return 0;
    }
    *out = LUFS_OFFSET + 10.0 * log10(energy);

    return *out > GATE_ABSOLUTE;
}

static int integrated_loudness(double *out) {
    double relative, energy = 0.0;
    double blocks = 0.0;
    int bin, first;

    if (!(nrm.gated_blocks > 0.0)) {
        return 0;
    }
    relative = LUFS_OFFSET + 10.0 * log10(nrm.gated_energy / nrm.gated_blocks) +
        GATE_RELATIVE;
    first = (int)ceil((relative - GATE_ABSOLUTE) / HIST_STEP);
    first = av_clip(first, 0, HIST_BINS - 1);
    for (bin = first; bin < HIST_BINS; bin++) {
        energy += nrm.hist_energy[bin];
        blocks += nrm.hist_count[bin];
    }
    if (!(blocks > 0.0) || !(energy > 0.0)) {
        energy = nrm.gated_energy;
        blocks = nrm.gated_blocks;
    }
    *out = LUFS_OFFSET + 10.0 * log10(energy / blocks);

    return 1;
}

static void push_sub_block(void) {
    double energy = 0.0;
    double loudness;
    int i, bin;

    nrm.sub[nrm.sub_head] = nrm.sub_energy;
    nrm.sub_head = (nrm.sub_head + 1) % SUBS_PER_SHORT_TERM;
    if (nrm.sub_count < SUBS_PER_SHORT_TERM) {
        nrm.sub_count++;
    }
    nrm.sub_energy = 0.0;
    nrm.sub_fill = 0;

    if (nrm.sub_count < SUBS_PER_BLOCK) {
        return;
    }
    for (i = 0; i < SUBS_PER_BLOCK; i++) {
        energy += nrm.sub[(nrm.sub_head - 1 - i + SUBS_PER_SHORT_TERM) % SUBS_PER_SHORT_TERM];
    }
    energy /= (double)SUBS_PER_BLOCK * nrm.sub_frames;
    if (!(energy > 0.0)) {
        return;
    }
    loudness = LUFS_OFFSET + 10.0 * log10(energy);
    if (loudness < GATE_ABSOLUTE) {
        return;
    }

    bin = av_clip((int)((loudness - GATE_ABSOLUTE) / HIST_STEP), 0, HIST_BINS - 1);
    nrm.hist_energy[bin] += energy;
    nrm.hist_count[bin]++;
    nrm.gated_energy += energy;
    nrm.gated_blocks += 1.0;
}

static int desired_gain(double *out_db, double *out_slew) {
    double recent, integrated, measured, confidence, share;
    int have_recent = window_loudness(SUBS_PER_SHORT_TERM, &recent);
    int have_integrated = integrated_loudness(&integrated);

    if (!have_recent && !have_integrated) {
        return 0;
    }

    confidence = (nrm.gated_blocks - CONFIDENCE_MIN_BLOCKS) /
        (CONFIDENCE_FULL_BLOCKS - CONFIDENCE_MIN_BLOCKS);
    confidence = av_clipd(confidence, 0.0, 1.0);

    if (!have_integrated) {
        measured = recent;
    } else if (!have_recent) {
        measured = integrated;
    } else {
        share = confidence * (1.0 - ADAPT_SHARE);
        measured = (1.0 - share) * recent + share * integrated;
    }

    SDL_SetAtomicInt(&reported_lufs, (int)lrint(measured * 100.0));

    *out_db = av_clipd(normalize_target_lufs() - measured,
                       NORMALIZE_CUT_MAX, NORMALIZE_BOOST_MAX);
    *out_slew = SLEW_SEEKING + (SLEW_SETTLED - SLEW_SEEKING) * confidence;

    return 1;
}

void normalize_process(int16_t *samples, int nb_frames, int nb_channels,
                       int sample_rate, const AVChannelLayout *ch_layout) {
    double frame[MAX_CHANNELS];
    double target_db = 0.0, slew = SLEW_SEEKING, target;
    double step_up, step_dn;
    double attack, release_fast, release_slow, hold_decay, ceiling;
    double gain, peak_hold, reduction;
    int enabled = normalize_enabled();
    int have_target, silent, bypass;
    size_t total, j;
    int i, ch;

    if (nb_frames <= 0 || !configure(sample_rate, nb_channels, ch_layout)) {
        return;
    }

    silent = 1;
    total = (size_t)nb_frames * (size_t)nb_channels;
    for (j = 0; j < total; j++) {
        if (samples[j]) {
            silent = 0;
            break;
        }
    }

    gain = nrm.gain;
    peak_hold = nrm.peak_hold;
    reduction = nrm.reduction;

    bypass = !enabled && gain == 1.0 && reduction >= 1.0;
    if (bypass && silent) {
        return;
    }

    have_target = desired_gain(&target_db, &slew);
    if (!enabled) {
        target_db = 0.0;
        slew = SLEW_TOGGLE;
        have_target = 1;
    } else if (SDL_GetAtomicInt(&normalize_toggling)) {
        slew = SLEW_TOGGLE;
    }
    target = pow(10.0, target_db / 20.0);

    step_up = pow(10.0, slew / (20.0 * sample_rate));
    step_dn = 1.0 / step_up;

    hold_decay = exp(-1.0 / (LIMIT_HOLD_S * sample_rate));
    attack = 1.0 - exp(-1.0 / (LIMIT_ATTACK_S * sample_rate));
    release_fast = 1.0 - exp(-1.0 / (LIMIT_RELEASE_FAST_S * sample_rate));
    release_slow = 1.0 - exp(-1.0 / (LIMIT_RELEASE_SLOW_S * sample_rate));
    ceiling = pow(10.0, CEILING_DB / 20.0);

    for (i = 0; i < nb_frames; i++) {
        int16_t *base = samples + (size_t)i * (size_t)nb_channels;
        double peak = 0.0;
        double energy = 0.0;
        double needed, coef, applied;

        for (ch = 0; ch < nb_channels; ch++) {
            double v = base[ch] * (1.0 / 32768.0);
            double a = fabs(v);

            frame[ch] = v;
            if (a > peak) {
                peak = a;
            }
        }

        if (!silent) {
            for (ch = 0; ch < nb_channels; ch++) {
                double y;

                if (nrm.weight[ch] == 0.0) {
                    continue;
                }
                y = biquad_run(&nrm.shelf, &nrm.shelf_state[ch], frame[ch]);
                y = biquad_run(&nrm.hpf, &nrm.hpf_state[ch], y);
                energy += nrm.weight[ch] * y * y;
            }
            nrm.sub_energy += energy;
            if (++nrm.sub_fill >= nrm.sub_frames) {
                push_sub_block();
            }
        }

        if (bypass) {
            continue;
        }

        if (have_target) {
            if (gain < target) {
                gain *= step_up;
                if (gain > target) {
                    gain = target;
                }
            } else if (gain > target) {
                gain *= step_dn;
                if (gain < target) {
                    gain = target;
                }
            }
        }

        peak *= gain;
        peak_hold *= hold_decay;
        if (peak > peak_hold) {
            peak_hold = peak;
        }
        needed = peak_hold > ceiling ? ceiling / peak_hold : 1.0;
        coef = needed < reduction
            ? attack
            : release_fast + (release_slow - release_fast) * (1.0 - reduction);
        reduction += (needed - reduction) * coef;
        if (reduction > 1.0 - 1e-9) {
            reduction = 1.0;
        }
        applied = gain * (reduction < needed ? reduction : needed);

        if (applied != 1.0) {
            double scale = applied * 32768.0;

            for (ch = 0; ch < nb_channels; ch++) {
                base[ch] = av_clip_int16((int)lrint(frame[ch] * scale));
            }
        }
    }

    nrm.gain = gain;
    nrm.peak_hold = peak_hold;
    nrm.reduction = reduction;
    if (have_target && enabled && fabs(gain - target) < 1e-9) {
        SDL_SetAtomicInt(&normalize_toggling, 0);
    }

    SDL_SetAtomicInt(&reported_gain, (int)lrint(2000.0 * log10(gain)));
    SDL_SetAtomicInt(&reported_reduction,
                     (int)lrint(-2000.0 * log10(FFMAX(reduction, 1e-6))));
}

void normalize_toggle(VideoState *is) {
    int on;

    if (!is->audio_st) {
        osd_show_message("No audio track");
        is->force_refresh = 1;
        return;
    }
    if (audio_spdif_active()) {
        osd_show_message("Normalize: unavailable while passing audio through");
        is->force_refresh = 1;
        return;
    }

    on = !normalize_enabled();
    SDL_SetAtomicInt(&normalize_toggling, 1);
    SDL_SetAtomicInt(&normalize_on, on);

    if (!on) {
        osd_show_message("Normalize: off");
    } else {
        int lufs = SDL_GetAtomicInt(&reported_lufs);
        double want = normalize_target_lufs();

        if (lufs == NO_MEASUREMENT) {
            osd_show_message("Normalize: on, %.1f LUFS target", want);
        } else {
            osd_show_message("Normalize: on, %+.1f dB to %.1f LUFS",
                             av_clipd(want - lufs / 100.0, NORMALIZE_CUT_MAX,
                                      NORMALIZE_BOOST_MAX),
                             want);
        }
    }
    is->force_refresh = 1;
}

const char *normalize_status(void) {
    static char status[192];
    int lufs, gain, reduction, n;

    if (audio_spdif_active()) {
        return "unavailable while passing audio through";
    }

    lufs = SDL_GetAtomicInt(&reported_lufs);
    gain = SDL_GetAtomicInt(&reported_gain);
    reduction = SDL_GetAtomicInt(&reported_reduction);

    if (!normalize_enabled()) {
        if (lufs == NO_MEASUREMENT) {
            return "off";
        }
        snprintf(status, sizeof(status), "off, source is %.1f LUFS",
                 lufs / 100.0);
        return status;
    }
    if (lufs == NO_MEASUREMENT) {
        return "on, measuring";
    }

    n = snprintf(status, sizeof(status),
                 "on, source %.1f LUFS, applying %+.1f dB toward %.1f LUFS",
                 lufs / 100.0, gain / 100.0, normalize_target_lufs());
    if (reduction > 10 && n > 0 && (size_t)n < sizeof(status)) {
        snprintf(status + n, sizeof(status) - (size_t)n, ", limiting %.1f dB",
                 reduction / 100.0);
    }

    return status;
}
