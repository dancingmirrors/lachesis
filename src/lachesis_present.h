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

#ifndef LACHESIS_PRESENT_H
#define LACHESIS_PRESENT_H

#include <stdint.h>

#define PRESENT_LEAD_MAX 0.004

enum PresentSource {
    PRESENT_SOURCE_SWAP = 0,
    PRESENT_SOURCE_PRESENT_WAIT,
    PRESENT_SOURCE_DISPLAY_TIMING,
};

const char *present_source_name(int source);

typedef struct PresentStats {
    double nominal_hz;
    double measured_hz;
    double jitter;
    int samples;
    int samples_needed;
    int measuring;
    int unsynced;
    int snapping;
    int locked;
    int source;
    int driver_refresh;
} PresentStats;

void present_update_display_mode(void);
void present_disable_snap(void);
void present_restore_snap(void);
void present_reset(void);

void present_feedback(int64_t submit_us, int64_t done_us);
void present_feedback_display(int source, int64_t display_us, double refresh_us);
void present_note_present(int64_t done_us);
void present_set_refresh_interval(double refresh_us);

double present_vsync_sec(void);

int64_t present_last_done_us(void);

double present_next_vsync(double now_sec, int *phase_locked);
double present_snap(double ideal_sec, double now_sec);

void present_get_stats(PresentStats *st);

#endif /* LACHESIS_PRESENT_H */
