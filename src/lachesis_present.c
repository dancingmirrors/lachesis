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

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_present.h"

#define MAX_VSYNC_SAMPLES 200
#define DELAY_VSYNC_SAMPLES 10
#define PRESENT_BLOCK_MIN_US 1000
#define PRESENT_MAX_GAP_US 1000000
#define PRESENT_MAX_FOLD 8
#define PRESENT_ANCHOR_STALE_US 1000000

static struct {
    double nominal_us;
    double interval_us;
    double estimated_us;
    double jitter;
    int use_estimated;

    int64_t samples[MAX_VSYNC_SAMPLES];
    int num_samples;
    int64_t num_total_samples;

    int64_t last_done_us;
    int64_t last_blocked_done_us;
    int num_successive;

    int snap_disabled;
} pres;

static double vsync_stddev(double ref_us) {
    double jitter = 0;
    for (int n = 0; n < pres.num_samples; n++) {
        double diff = pres.samples[n] - ref_us;
        jitter += diff * diff;
    }
    return sqrt(jitter / pres.num_samples);
}

static void check_estimated_display_fps(void) {
    int use_estimated = 0;

    if (pres.num_total_samples >= MAX_VSYNC_SAMPLES / 2 &&
        pres.estimated_us <= 1e6 / 20.0 && pres.estimated_us >= 1e6 / 400.0) {
        use_estimated = 1;
        for (int n = 0; n < pres.num_samples; n++) {
            if (fabs(pres.samples[n] - pres.estimated_us) >= pres.estimated_us / 4) {
                use_estimated = 0;
                break;
            }
        }
        if (use_estimated && pres.nominal_us > 0) {
            double mjitter = vsync_stddev(pres.estimated_us);
            double njitter = vsync_stddev(pres.nominal_us);
            if (mjitter * 1.01 >= njitter) {
                use_estimated = 0;
            }
        }
    }

    if (use_estimated != pres.use_estimated) {
        if (use_estimated) {
            log_verbose("Adjusting display FPS to a measured %.3f Hz.\n",
                        1e6 / pres.estimated_us);
        } else if (pres.nominal_us > 0) {
            log_verbose("Switching back to the reported display FPS of %.3f Hz.\n",
                        1e6 / pres.nominal_us);
        }
        pres.use_estimated = use_estimated;
    }

    pres.interval_us = pres.use_estimated ? pres.estimated_us : pres.nominal_us;
}

void present_update_display_mode(void) {
    double hz = 0;

    if (display_fps_override > 0) {
        hz = display_fps_override;
    } else if (window) {
        SDL_DisplayID id = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode = id ? SDL_GetCurrentDisplayMode(id) : NULL;
        if (mode) {
            if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
                hz = (double)mode->refresh_rate_numerator / mode->refresh_rate_denominator;
            } else if (mode->refresh_rate > 0) {
                hz = mode->refresh_rate;
            }
        }
    }

    double nominal_us = hz > 0 ? 1e6 / hz : 0;
    if (nominal_us != pres.nominal_us) {
        pres.nominal_us = nominal_us;
        pres.num_samples = 0;
        pres.num_total_samples = 0;
        pres.estimated_us = 0;
        pres.use_estimated = 0;
        pres.jitter = 0;
        present_reset();
        check_estimated_display_fps();
    }
}

void present_disable_snap(void) {
    pres.snap_disabled = 1;
}

void present_restore_snap(void) {
    pres.snap_disabled = 0;
}

void present_reset(void) {
    pres.num_successive = 0;
    pres.last_done_us = 0;
    pres.last_blocked_done_us = 0;
}

void present_feedback(int64_t submit_us, int64_t done_us) {
    int64_t prev_done = pres.last_done_us;
    int64_t prev_blocked = pres.last_blocked_done_us;
    int blocked = done_us - submit_us >= PRESENT_BLOCK_MIN_US;

    if (done_us <= 0 || done_us < submit_us) {
        return;
    }
    pres.last_done_us = done_us;
    if (blocked) {
        pres.last_blocked_done_us = done_us;
    }

    if (prev_done <= 0 || done_us - prev_done > PRESENT_MAX_GAP_US) {
        pres.num_successive = 0;
        return;
    }

    pres.num_successive++;
    if (pres.num_successive <= DELAY_VSYNC_SAMPLES) {
        return;
    }

    if (!blocked || prev_blocked <= 0 || done_us - prev_blocked > PRESENT_MAX_GAP_US) {
        return;
    }

    double ref_us = pres.interval_us;
    int64_t delta = done_us - prev_blocked;
    int64_t folds = 1;
    if (ref_us > 0) {
        folds = llrint(delta / ref_us);
        if (folds < 1 || folds > PRESENT_MAX_FOLD) {
            return;
        }
        if (fabs(delta - folds * ref_us) >= ref_us / 4) {
            return;
        }
    } else if (delta < 1e6 / 400.0 || delta > 1e6 / 20.0) {
        return;
    }

    if (pres.num_samples >= MAX_VSYNC_SAMPLES) {
        pres.num_samples -= 1;
    }
    memmove(&pres.samples[1], &pres.samples[0],
            pres.num_samples * sizeof(pres.samples[0]));
    pres.samples[0] = (int64_t)llrint((double)delta / folds);
    pres.num_samples++;
    pres.num_total_samples++;

    double avg = 0;
    for (int n = 0; n < pres.num_samples; n++) {
        avg += pres.samples[n];
    }
    pres.estimated_us = avg / pres.num_samples;
    if (pres.interval_us > 0) {
        pres.jitter = vsync_stddev(pres.interval_us) / pres.interval_us;
    }

    check_estimated_display_fps();
}

double present_vsync_sec(void) {
    return pres.interval_us > 0 ? pres.interval_us / 1e6 : 0;
}

int64_t present_last_done_us(void) {
    return pres.last_done_us;
}

double present_snap(double ideal_sec, double now_sec) {
    double vsync = present_vsync_sec();

    if (pres.snap_disabled || vsync <= 0 || pres.last_done_us <= 0) {
        return ideal_sec;
    }
    if (ideal_sec <= now_sec) {
        return ideal_sec;
    }

    double anchor = pres.last_done_us / 1e6;
    if (now_sec - anchor > PRESENT_ANCHOR_STALE_US / 1e6) {
        return ideal_sec;
    }

    double target = anchor + round((ideal_sec - anchor) / vsync) * vsync;
    if (target <= now_sec) {
        return ideal_sec;
    }

    return target;
}

void present_get_stats(PresentStats *st) {
    memset(st, 0, sizeof(*st));
    st->nominal_hz = pres.nominal_us > 0 ? 1e6 / pres.nominal_us : 0;
    st->measured_hz = pres.estimated_us > 0 ? 1e6 / pres.estimated_us : 0;
    st->jitter = pres.jitter;
    st->measuring = pres.use_estimated;
    st->snapping = !pres.snap_disabled && pres.interval_us > 0;
}
